#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/modelRender.h"
#include "core2/anctrl.h"
#include "core2/commonParticle.h"

extern s32 commonParticle_new(enum common_particle_e particle_id, s32 arg1);
extern void commonParticle_add(s32 actorMarker, s32 arg1, s32 arg2);
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
    f32 x, y, z, yaw, pitch, scale;
    u32 map_id;
    u16 animation_id;
    u8  _pad1[2];
    f32 anim_timer, anim_duration;
    u8  anim_playback_type, health, health_total, lives, transformation, bs_state;
    u8  _pad2[2];
    f32 horizontal_velocity;
} RemoteState;

extern void *baModelBin;
extern f32 baModelScale;
extern void func_8029A47C(s32 env_color[3]);
extern void func_8033A280(f32);
extern struct5Bs *D_80363780;
extern void func_8033A450(struct5Bs *);
extern void baanim_80289F30(void);
extern void func_8029DD6C(void);
extern AnimCtrl *baanim_getAnimCtrlPtr(void);
extern Animation *anctrl_getAnimPtr(AnimCtrl *this);
extern void *animcache_getCurrentTransform(Animation *this);
extern enum asset_e baModel_getModelId(void);
extern void bkrecomp_setup_custom_skinning(ModelSkinningData* skinning_data, u32 model_id);

extern void *animBinCache_get(enum asset_e asset_id);
// func_8033A444 declared in core2/modelRender.h — sets D_8038371C (AnimMtxList)
extern void animationFile_getBoneTransformList(void *anim_file, f32 progress, void *bone_list);
extern void *boneTransformList_new(void);
extern void modelRender_setBoneTransformList(void *bone_list);
extern void boneTransformList_interpolate(void *result, void *start, void *end, f32 t);
extern f32 time_getDelta(void);
extern f32 mapModel_getFloorY(f32 pos[3]);

// Dust puff: commonParticle type 7 (from code_CBD10.c func_80352CF4)
// Used for walking dust AND beak buster impact (different scale params)
extern void func_80352CF4(f32 pos[3], f32 vel[3], f32 startScale, f32 endScale);
// Angle to velocity vector (from code_A2B0.c)
extern void func_802589E4(f32 dst[3], f32 angle, f32 magnitude);
// Normalize angle
extern f32 mlNormalizeAngle(f32 angle);

#define MAX_PLAYERS 4
#define BLEND_DURATION 0.15f  // 150ms blend between animations

typedef struct {
    void *shadow_model;
    void *bone_current;   // Current animation bones
    void *bone_prev;      // Previous animation bones (for blending)
    void *bone_blend;     // Blended result
    u16 current_anim;
    u16 prev_anim;
    f32 smooth_yaw;
    f32 ghost_timer;
    f32 anim_duration;
    f32 anim_start;      // subRange start (0.0-1.0)
    f32 anim_end;        // subRange end (0.0-1.0)
    f32 blend_timer;      // 0.0 = fully prev, BLEND_DURATION = fully current
    f32 ground_y;
    f32 prev_y;           // Previous frame Y for descent detection
    f32 bflap_elapsed;    // Bflap: total elapsed time in state
    u8 prev_bs_state;
    u8 dust_cooldown;
    u8 bflap_count;       // Bflap: flap counter (0-4)
    u8 idle_phase;        // Idle: cycle index (0-20) into sequence table
    f32 bpeck_timer;      // Bpeck: countdown for pecking loop phase (0.5s)
    bool bflap_released;  // Bflap: A button released (detected via fast descent)
    bool kazooie_visible; // Whether Kazooie HEAD should be rendered (D_8037D238)
    bool kazooie_body;    // Whether Kazooie WINGS should be rendered (D_8037D236)
    bool kazooie_feet;    // Whether Kazooie FEET should be rendered (D_8037D235)
    bool anim_loops;
    bool blending;
    bool initialized;
} GhostModel;

static GhostModel ghost_models[MAX_PLAYERS] = {0};
static ModelSkinningData ghost_skinning_data[MAX_PLAYERS] = {0};

static f32 lerp_angle(f32 current, f32 target, f32 speed) {
    f32 diff = target - current;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    current += diff * speed;
    while (current >= 360.0f) current -= 360.0f;
    while (current < 0.0f) current += 360.0f;
    return current;
}

// Ghost equivalent of func_8029DD6C — sets up model node display list indices.
// Must be called before each ghost modelRender_draw to avoid inheriting
// local player's node state (which can cause T-pose or wrong K visibility).
extern void func_8033A1FC(void);  // Clears all 42 D_80383658 entries to 0

static void ghost_setup_model_nodes(bool kazooie_head, bool kazooie_wings, bool kazooie_feet) {
    // DON'T call func_8033A1FC (clear all to 0) — it destroys nodes the model
    // needs for arm/body rendering. Instead, inherit the local player's node state
    // and only override Kazooie-specific nodes for the ghost's K visibility.

    // K head/beak (D_8037D238)
    s32 k_h = kazooie_head ? 1 : 0;
    func_8033A45C(1, k_h);
    func_8033A45C(9, k_h);
    func_8033A45C(0xC, k_h);
    func_8033A45C(0xF, k_h);

    // K wings (D_8037D236) — ONLY bTrot, bLongLeg, bShock
    s32 k_w = kazooie_wings ? 1 : 0;
    func_8033A45C(2, k_w);
    func_8033A45C(0xA, k_w);
    func_8033A45C(0xD, k_w);
    func_8033A45C(0x10, k_w);

    // K feet (D_8037D235) — ONLY bEggAss
    s32 k_f = kazooie_feet ? 1 : 0;
    func_8033A45C(8, k_f);
    func_8033A45C(0xB, k_f);
    func_8033A45C(0xE, k_f);
    func_8033A45C(0x11, k_f);
}

// Prepare bone_prev with current pose before switching to a new animation.
// This guarantees bone_prev has clean data for blending, preventing stale
// data from persisting across multiple transitions.
static void ghost_prepare_blend(GhostModel *gm) {
    void *anim_file = animBinCache_get(gm->current_anim);
    if (anim_file) {
        animationFile_getBoneTransformList(anim_file, gm->ghost_timer, gm->bone_prev);
    }
    gm->blend_timer = 0.0f;
    gm->blending = TRUE;
}

static void ghost_ensure_init(u32 pid) {
    GhostModel *gm = &ghost_models[pid];
    if (gm->initialized) return;

    gm->shadow_model = assetcache_get(ASSET_3BF_MODEL_PLAYER_SHADOW);
    gm->bone_current = boneTransformList_new();
    gm->bone_prev = boneTransformList_new();
    gm->bone_blend = boneTransformList_new();
    if (!gm->bone_current || !gm->bone_prev || !gm->bone_blend) return;

    void *anim_file = animBinCache_get(ASSET_6F_ANIM_BSSTAND_IDLE);
    if (anim_file) {
        animationFile_getBoneTransformList(anim_file, 0.0f, gm->bone_current);
        animationFile_getBoneTransformList(anim_file, 0.0f, gm->bone_prev);
        animationFile_getBoneTransformList(anim_file, 0.0f, gm->bone_blend);
    }

    gm->current_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->prev_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->smooth_yaw = 0.0f;
    gm->ghost_timer = 0.0f;
    gm->anim_duration = 5.5f;
    gm->anim_start = 0.0f;
    gm->anim_end = 1.0f;
    gm->ground_y = 0.0f;
    gm->prev_y = 0.0f;
    gm->blend_timer = BLEND_DURATION;
    gm->blending = FALSE;
    gm->anim_loops = FALSE;  // Idle starts ONCE (advances through idle cycle phases)
    gm->initialized = TRUE;
}

// Map velocity to duration using the game's velocity map ranges (from walk.c)
static f32 velocity_to_duration(f32 vel, f32 vel_min, f32 vel_max, f32 dur_slow, f32 dur_fast) {
    if (vel <= vel_min) return dur_slow;
    if (vel >= vel_max) return dur_fast;
    f32 t = (vel - vel_min) / (vel_max - vel_min);
    return dur_slow + (dur_fast - dur_slow) * t;
}

static void ghost_sync_anim(GhostModel *gm, u8 bs_state, f32 h_velocity) {
    if (!gm->initialized) return;

    u16 anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    f32 duration = 5.5f;
    bool loops = TRUE;

    switch (bs_state) {
        // Idle: ONCE first cycle, then cycles through idle/kazooie-peck sequence
        // Don't reset if already in an idle animation phase
        case BS_0_NONE: case BS_1_IDLE:
            if (gm->current_anim == ASSET_6F_ANIM_BSSTAND_IDLE
                || gm->current_anim == ASSET_95_ANIM_BSSTAND_KAZOOIE_PECK
                || gm->current_anim == ASSET_F6_ANIM_BSSTAND_PULL_KAZOOIE) {
                return;
            }
            anim = ASSET_6F_ANIM_BSSTAND_IDLE; duration = 5.5f; loops = FALSE;
            break;
        case BS_20_LANDING:
            // Landing keeps previous animation (bsstand_landing_init doesn't change anim)
            // BUT: if ghost is stuck in a fall/aerial animation due to network latency,
            // force transition to landing pose to avoid arms-open freeze
            if (gm->current_anim == ASSET_B0_ANIM_BSJUMP_FALL
                || gm->current_anim == ASSET_68_ANIM_BSJUMP_TUMBLE
                || gm->current_anim == ASSET_17_ANIM_BSBFLAP
                || gm->current_anim == ASSET_18_ANIM_BSBFLAP_ENTER
                || gm->current_anim == ASSET_4C_ANIM_BSBFLIP_HOLD
                || gm->current_anim == ASSET_61_ANIM_BSBFLIP_EXIT) {
                // Force to BSJUMP landing pose (same as fall near-ground transition)
                anim = ASSET_8_ANIM_BSJUMP; duration = 2.0f; loops = FALSE;
                break;
            }
            return;
        case BS_D_TIMEOUT: case BS_53_TIMEOUT:
            anim = ASSET_77_ANIM_BSTIMEOUT; duration = 3.2f; loops = TRUE; break;
        // Walk states: all LOOP, duration scaled by horizontal velocity (walk.c)
        case BS_WALK_CREEP:
            anim = ASSET_2_ANIM_BSWALK_CREEP;
            duration = velocity_to_duration(h_velocity, 30.0f, 80.0f, 1.8f, 1.2f);
            break;
        case BS_2_WALK_SLOW:
            anim = ASSET_3_ANIM_BSWALK;
            duration = velocity_to_duration(h_velocity, 80.0f, 150.0f, 1.3f, 0.6f);
            break;
        case BS_WALK:
            anim = ASSET_C_ANIM_BSWALK_RUN;
            duration = velocity_to_duration(h_velocity, 150.0f, 225.0f, 0.92f, 0.58f);
            break;
        case BS_4_WALK_FAST:
            anim = ASSET_C_ANIM_BSWALK_RUN;
            duration = velocity_to_duration(h_velocity, 225.0f, 500.0f, 0.54f, 0.44f);
            break;
        case BS_SKID:  anim = ASSET_E_ANIM_BSTURN; duration = 0.3f; loops = FALSE; break;
        case BS_SLIDE: anim = ASSET_5A_ANIM_BSSLIDE_FRONT; duration = 1.0f; break;
        case BS_ROLL:  anim = ASSET_4F_ANIM_BSTWIRL; duration = 0.9f; loops = TRUE; break;
        // Jump: subRange 0.3→0.5042 at 1.9f ONCE (from bs/jump.c bsjump_init)
        case BS_5_JUMP: anim = ASSET_8_ANIM_BSJUMP; duration = 1.9f; loops = FALSE; break;
        // Bflip (Z+A somersault): multi-phase animation
        // Phases managed in per-frame section. Don't reset if already in a bflip phase.
        case BS_12_BFLIP:
            if (gm->current_anim == ASSET_4B_ANIM_BSBFLIP_ENTER
                || gm->current_anim == ASSET_4C_ANIM_BSBFLIP_HOLD
                || gm->current_anim == ASSET_61_ANIM_BSBFLIP_EXIT) {
                return;
            }
            anim = ASSET_4B_ANIM_BSBFLIP_ENTER; duration = 2.3f; loops = FALSE;
            break;
        case BS_2F_FALL: anim = ASSET_B0_ANIM_BSJUMP_FALL; duration = 0.38f; loops = TRUE; break;
        case BS_3D_FALL_TUMBLING: anim = ASSET_68_ANIM_BSJUMP_TUMBLE; duration = 0.35f; loops = TRUE; break;
        case BS_CLAW:  anim = ASSET_5_ANIM_BSPUNCH; duration = 1.3f; loops = FALSE; break;
        // Bbuster: multi-phase, managed in per-frame section
        case BS_F_BBUSTER:
            if (gm->current_anim == ASSET_1D_ANIM_BSBBUSTER) {
                return; // Already in bbuster phase, don't reset
            }
            anim = ASSET_1D_ANIM_BSBBUSTER; duration = 1.02f; loops = FALSE;
            break;
        // Bflap (feathery flap, A in air): ENTER 0.3f ONCE → BSBFLAP loop (speeds up)
        // Phases managed in per-frame section. Don't reset if already in a bflap phase.
        case BS_BFLAP:
            if (gm->current_anim == ASSET_18_ANIM_BSBFLAP_ENTER
                || gm->current_anim == ASSET_17_ANIM_BSBFLAP) {
                return;
            }
            anim = ASSET_18_ANIM_BSBFLAP_ENTER; duration = 0.30f; loops = FALSE;
            break;
        // Bpeck (A+B): BSBPECK 0.2f ONCE → at 91%: BSBPECK_ENTER loop 0.35f for 0.5s → BSBPECK retract
        case BS_11_BPECK:
            if (gm->current_anim == ASSET_1A_ANIM_BSBPECK
                || gm->current_anim == ASSET_19_ANIM_BSBPECK_ENTER) {
                return;
            }
            anim = ASSET_1A_ANIM_BSBPECK; duration = 0.2f; loops = FALSE;
            break;
        case BS_BBARGE: anim = ASSET_1C_ANIM_BSBBARGE; duration = 1.0f; loops = FALSE; break;
        case BS_CROUCH: anim = ASSET_1_ANIM_BSCROUCH_ENTER; duration = 0.5f; loops = FALSE; break;
        case BS_9_EGG_HEAD: anim = ASSET_2A_ANIM_BSEGGHEAD; duration = 1.0f; loops = FALSE; break;
        case BS_A_EGG_ASS: anim = ASSET_2B_ANIM_BSEGGASS; duration = 1.0f; loops = FALSE; break;
        case BS_14_BTROT_ENTER: anim = ASSET_26_ANIM_BSBTROT_IDLE; duration = 1.2f; loops = TRUE; break;
        case BS_15_BTROT_IDLE: anim = ASSET_26_ANIM_BSBTROT_IDLE; duration = 1.2f; loops = TRUE; break;
        case BS_16_BTROT_WALK: anim = ASSET_15_ANIM_BSBTROT_WALK; duration = 0.57f; loops = TRUE; break;
        case BS_17_BTROT_EXIT: anim = ASSET_7_ANIM_BSBTROT_EXIT; duration = 0.6f; loops = FALSE; break;
        case BS_8_BTROT_JUMP: anim = ASSET_27_ANIM_BSBTROR_JUMP; duration = 1.4f; loops = FALSE; break;
        case BS_1A_WONDERWING_ENTER:
            anim = ASSET_22_ANIM_BSWHIRL_EXIT; duration = 0.5f; loops = FALSE; break;
        case BS_1B_WONDERWING_IDLE:
            anim = ASSET_23_ANIM_BSWONDERWING_IDLE; duration = 1.0f; break;
        case BS_1C_WONDERWING_WALK:
            anim = ASSET_11_ANIM_BSWHIRL_WALK; duration = 0.6f; break;
        case BS_1D_WONDERWING_JUMP:
            anim = ASSET_23_ANIM_BSWONDERWING_IDLE; duration = 1.0f; break;
        case BS_1E_WONDERWING_EXIT:
            anim = ASSET_22_ANIM_BSWHIRL_EXIT; duration = 0.5f; loops = FALSE; break;
        case BS_23_FLY_ENTER: anim = ASSET_45_ANIM_BSBFLY_ENTER; duration = 1.4f; loops = FALSE; break;
        case BS_24_FLY: anim = ASSET_38_ANIM_BSBFLY; duration = 0.62f; break;
        case BS_18_FLY_KNOCKBACK: case BS_FLY_OW: case BS_58_BEAKBOMB_CRASH:
            anim = ASSET_3E_ANIM_BSBFLY_BEAKBOMB_CRASH; duration = 1.4f; loops = FALSE; break;
        case BS_BOMB: anim = ASSET_43_ANIM_BSBFLY_BEAKBOMB_START; duration = 1.0f; loops = FALSE; break;
        case BS_2D_SWIM_IDLE: anim = ASSET_57_ANIM_BSSWIM_IDLE; duration = 1.2f; break;
        case BS_2E_SWIM: anim = ASSET_39_ANIM_BSSWIM_MOVE; duration = 0.75f; break;
        case BS_30_DIVE_ENTER: anim = ASSET_3C_ANIM_BSSWIM_DIVE_ENTER; duration = 1.0f; loops = FALSE; break;
        case BS_2B_DIVE_IDLE: anim = ASSET_70_ANIM_BSSWIM_DIVE_IDLE; duration = 2.0f; break;
        case BS_2C_DIVE_B: case BS_39_DIVE_A:
            anim = ASSET_3F_ANIM_BSSWIM_DIVE_MOVE; duration = 0.75f; break;
        case BS_54_SWIM_DIE: anim = ASSET_B9_ANIM_BSSWIM_DIE; duration = 0.7f; loops = FALSE; break;
        case BS_4F_CLIMB_IDLE: anim = ASSET_B2_ANIM_BSCLIMB_IDLE_2; duration = 2.64f; break;
        case BS_50_CLIMB_MOVE: anim = ASSET_A_ANIM_BSCLIMB_MOVE; duration = 0.9f; break;
        case BS_25_LONGLEG_ENTER: case BS_26_LONGLEG_IDLE: case BS_LONGLEG_EXIT:
            anim = ASSET_41_ANIM_BSLONGLEG_IDLE; duration = 1.0f; break;
        case BS_LONGLEG_WALK: anim = ASSET_42_ANIM_BSLONGLEG_WALK; duration = 0.53f; break;
        case BS_LONGLEG_JUMP: anim = ASSET_3D_ANIM_BSLONGLEG_JUMP; duration = 1.5f; loops = FALSE; break;
        case BS_3A_CARRY_IDLE: anim = ASSET_72_ANIM_BSCARRY_IDLE; duration = 5.5f; break;
        case BS_3B_CARRY_WALK: anim = ASSET_73_ANIM_BSCARRY_WALK; duration = 0.7f; break;
        case BS_CARRY_THROW: anim = ASSET_11B_ANIM_BSTHROW; duration = 0.8f; loops = FALSE; break;
        case BS_E_OW: anim = ASSET_4D_ANIM_BSOW; duration = 1.0f; loops = FALSE; break;
        case BS_56_RECOIL: anim = ASSET_F_ANIM_BSREBOUND; duration = 1.0f; loops = FALSE; break;
        case BS_41_DIE: anim = ASSET_9_ANIM_BSDIE; duration = 3.0f; loops = FALSE; break;
        case BS_SPLAT: anim = ASSET_D2_ANIM_BSSPLAT; duration = 2.25f; loops = FALSE; break;
        case BS_3C_TALK: anim = ASSET_14A_ANIM_BSREST_LISTEN; duration = 11.4f; break;
        case BS_44_JIG_JIGGY: anim = ASSET_2E_ANIM_BSJIG_JIGGY; duration = 2.0f; loops = FALSE; break;
        case BS_35_ANT_IDLE: anim = ASSET_5E_ANIM_BSANT_IDLE; duration = 1.2f; break;
        case BS_ANT_WALK: anim = ASSET_5F_ANIM_BSANT_WALK; duration = 0.8f; break;
        case BS_ANT_JUMP: case BS_38_ANT_FALL:
            anim = ASSET_60_ANIM_BSANT_JUMP; duration = 1.5f; loops = TRUE; break;
        case BS_48_PUMPKIN_IDLE: anim = ASSET_5E_ANIM_BSANT_IDLE; duration = 1.2f; break;
        case BS_49_PUMPKIN_WALK: case BS_4B_PUMPKIN_FALL:
            anim = ASSET_A0_ANIM_BSPUMPKIN_WALK; duration = 0.8f; break;
        case BS_5E_CROC_IDLE: anim = ASSET_E1_ANIM_BSCROC_IDLE; duration = 1.0f; break;
        case BS_CROC_WALK: anim = ASSET_E0_ANIM_BSCROC_WALK; duration = 0.8f; break;
        case BS_67_WALRUS_IDLE: anim = ASSET_11F_ANIM_BSWALRUS_IDLE; duration = 4.0f; break;
        case BS_WALRUS_WALK: anim = ASSET_120_ANIM_BSWALRUS_WALK; duration = 0.8f; break;
        case BS_85_BEE_IDLE: anim = ASSET_1DE_ANIM_BEE_IDLE; duration = 3.0f; break;
        case BS_BEE_WALK: anim = ASSET_1DD_ANIM_BEE_WALK; duration = 0.38f; break;
        case BS_BEE_FLY: anim = ASSET_1DC_ANIM_BEE_FLY; duration = 0.38f; break;
        default: anim = ASSET_6F_ANIM_BSSTAND_IDLE; duration = 5.5f; loops = TRUE; break;
    }

    // Reset when animation changes OR when bs_state changes (even if same anim asset).
    // Needed for: BS_20_LANDING keeps jump anim → next BS_5_JUMP must restart phase 0.
    if (anim != gm->current_anim || bs_state != gm->prev_bs_state) {
        // Same animation but different state (e.g. WALK↔FAST both use ASSET_C):
        // Just update duration, keep timer position (game carries over timer in walk.c)
        // Exception: states that MUST restart even with same anim (e.g. LANDING→JUMP)
        if (anim == gm->current_anim && bs_state != BS_5_JUMP) {
            gm->anim_duration = duration;
            gm->anim_loops = loops;
            return;
        }

        // recomp_printf("[GhostAnim] BS=0x%02X anim=0x%03X dur=%.2f %s\n", bs_state, anim, duration, loops ? "LOOP" : "ONCE");

        ghost_prepare_blend(gm);

        gm->prev_anim = gm->current_anim;
        gm->current_anim = anim;
        gm->anim_duration = duration;
        gm->anim_loops = loops;

        // Reset idle phase when entering idle
        if ((bs_state == BS_1_IDLE || bs_state == BS_0_NONE)
            && gm->prev_bs_state != BS_1_IDLE && gm->prev_bs_state != BS_0_NONE) {
            gm->idle_phase = 0;
        }

        // Reset bpeck state when entering bpeck
        if (bs_state == BS_11_BPECK && gm->prev_bs_state != BS_11_BPECK) {
            gm->bpeck_timer = 0.0f;
        }

        // Reset bflap state when entering bflap
        if (bs_state == BS_BFLAP && gm->prev_bs_state != BS_BFLAP) {
            gm->bflap_count = 0;
            gm->bflap_elapsed = 0.0f;
            gm->bflap_released = FALSE;
        }

        // Set subRange based on game source
        if (anim == ASSET_8_ANIM_BSJUMP && bs_state == BS_5_JUMP) {
            // bs/jump.c bsjump_init: subRange 0.3→0.5042, start at 0.3
            gm->anim_start = 0.3f;
            gm->anim_end = 0.5042f;
            gm->ghost_timer = 0.3f;
        } else if (anim == ASSET_8_ANIM_BSJUMP && bs_state == BS_20_LANDING) {
            // Landing from fall: start at landing pose (0.6667)
            gm->anim_start = 0.0f;
            gm->anim_end = 1.0f;
            gm->ghost_timer = 0.6667f;
        } else if (anim == ASSET_1D_ANIM_BSBBUSTER && bs_state == BS_F_BBUSTER) {
            // bbuster.c bsbbuster_init: subRange 0.0→0.35, dur 1.02f
            gm->anim_start = 0.0f;
            gm->anim_end = 0.35f;
            gm->ghost_timer = 0.0f;
        } else if (anim == ASSET_4B_ANIM_BSBFLIP_ENTER && bs_state == BS_12_BFLIP) {
            // bFlip.c bsbflip_init: subRange 0.0→0.7866, dur 2.3f
            gm->anim_start = 0.0f;
            gm->anim_end = 0.7866f;
            gm->ghost_timer = 0.0f;
        } else {
            gm->anim_start = 0.0f;
            gm->anim_end = 1.0f;
            gm->ghost_timer = 0.0f;
        }
    }
}

void bkrecomp_net_draw_ghosts(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    if (!recomp_net_is_connected()) return;
    if (!baModelBin) return;

    u32 local_id = recomp_net_get_local_player_id();
    u32 local_map = (u32)map_get();

    for (u32 pid = 0; pid < MAX_PLAYERS; pid++) {
        if (pid == local_id) continue;

        RemoteState rs;
        if (!recomp_net_get_remote_state(pid, &rs)) continue;
        if (rs.map_id != local_map || rs.x < -15000.0f) continue;

        ghost_ensure_init(pid);
        GhostModel *gm = &ghost_models[pid];
        if (!gm->initialized) continue;

        ghost_sync_anim(gm, rs.bs_state, rs.horizontal_velocity);

        // Continuously scale walk animation duration based on velocity (every frame)
        // Mirrors baanim_scaleDuration() called in every walk update function
        if (rs.bs_state == BS_WALK_CREEP) {
            gm->anim_duration = velocity_to_duration(rs.horizontal_velocity, 30.0f, 80.0f, 1.8f, 1.2f);
        } else if (rs.bs_state == BS_2_WALK_SLOW) {
            gm->anim_duration = velocity_to_duration(rs.horizontal_velocity, 80.0f, 150.0f, 1.3f, 0.6f);
        } else if (rs.bs_state == BS_WALK) {
            gm->anim_duration = velocity_to_duration(rs.horizontal_velocity, 150.0f, 225.0f, 0.92f, 0.58f);
        } else if (rs.bs_state == BS_4_WALK_FAST) {
            gm->anim_duration = velocity_to_duration(rs.horizontal_velocity, 225.0f, 500.0f, 0.54f, 0.44f);
        }

        // === Ground tracking + dust particles ===
        // Walking dust uses commonParticle system (type 6) via func_8029CDC0
        // Beak buster impact uses commonParticle type 0xB via func_80354030
        // Skid/run dust uses commonParticle type 0xE via func_80354380
        {
            f32 ghost_pos[3] = {rs.x, rs.y, rs.z};
            f32 floor_y = mapModel_getFloorY(ghost_pos);
            f32 height = rs.y - floor_y;
            bool on_ground = (height >= -10.0f && height < 30.0f);
            if (on_ground) gm->ground_y = floor_y;

            if (gm->dust_cooldown > 0) gm->dust_cooldown--;

            bool running = (rs.bs_state == BS_4_WALK_FAST || rs.bs_state == BS_WALK);
            bool walking = (rs.bs_state == BS_2_WALK_SLOW || rs.bs_state == BS_WALK_CREEP);

            // Dust events (commonParticle type 7 = brown/gray puff)
            // All dust uses commonParticle type 7 via func_80352CF4
            // Parameters from: code_B850.c, bs/bBuster.c, bs/slide.c, bs/bBarge.c, bs/crouch.c
            bool start_moving = ((running || walking) &&
                (gm->prev_bs_state == BS_1_IDLE || gm->prev_bs_state == BS_0_NONE
                || gm->prev_bs_state == BS_20_LANDING));
            bool direction_change = (rs.bs_state == BS_SKID);
            bool bbuster_land = (rs.bs_state == BS_20_LANDING && gm->prev_bs_state == BS_F_BBUSTER);
            bool is_sliding = (rs.bs_state == BS_SLIDE);
            bool is_barge = (rs.bs_state == BS_BBARGE);
            bool btrot_start = (rs.bs_state == BS_16_BTROT_WALK &&
                (gm->prev_bs_state == BS_15_BTROT_IDLE || gm->prev_bs_state == BS_14_BTROT_ENTER));
            bool btrot_walking = (rs.bs_state == BS_16_BTROT_WALK);

            f32 dust_pos[3] = {rs.x, rs.y + 10.0f, rs.z};

            if (on_ground && gm->dust_cooldown == 0) {
                if (bbuster_land) {
                    // Beak buster: 12 puffs circular (bBuster.c func_8029FB30)
                    f32 i;
                    for (i = 0.0f; i < 359.0f; i += 60.0f) {
                        f32 vel[3];
                        func_802589E4(vel, i, 730.0f * 0.51f);
                        vel[1] = 100.0f;
                        func_80352CF4(dust_pos, vel, 150.0f, 10.0f);
                    }
                    for (i = 0.0f; i < 359.0f; i += 60.0f) {
                        f32 vel[3];
                        func_802589E4(vel, mlNormalizeAngle(i + 30.0f), 430.0f * 0.51f);
                        vel[1] = 40.0f;
                        func_80352CF4(dust_pos, vel, 150.0f, 10.0f);
                    }
                    gm->dust_cooldown = 15;
                } else if (is_sliding) {
                    // Slide dust: zigzag trail (bs/slide.c func_802B40D0)
                    // Alternates offset left/right/center using yaw+90
                    static s32 slide_phase = 0;
                    f32 slide_pos[3] = {rs.x, rs.y + 20.0f, rs.z};
                    slide_phase++;
                    if (slide_phase >= 3) slide_phase = 0;
                    if (slide_phase != 0) {
                        f32 side_offset[3];
                        f32 side_angle = mlNormalizeAngle(rs.yaw + 90.0f);
                        func_802589E4(side_offset, side_angle, randf() * 10.0f + 20.0f);
                        side_offset[1] = 0.0f;
                        if (slide_phase == 1) {
                            slide_pos[0] -= side_offset[0];
                            slide_pos[2] -= side_offset[2];
                        } else {
                            slide_pos[0] += side_offset[0];
                            slide_pos[2] += side_offset[2];
                        }
                    }
                    f32 vel[3];
                    func_802589E4(vel, rs.yaw, 40.0f);
                    vel[1] = 50.0f;
                    func_80352CF4(slide_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 4;
                } else if (is_barge) {
                    // Barge (Z+B): dust trail (bs/bBarge.c)
                    f32 vel[3] = {0.0f, 40.0f, 0.0f};
                    f32 barge_pos[3];
                    func_802589E4(barge_pos, rs.yaw - 20.0f, 20.0f);
                    barge_pos[0] += rs.x; barge_pos[1] = rs.y + 10.0f; barge_pos[2] += rs.z;
                    func_80352CF4(barge_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 4;
                } else if (direction_change) {
                    // Turn/skid dust (bs/turn.c func_802927E0)
                    f32 vel[3];
                    func_802589E4(vel, rs.yaw, 200.0f * 0.51f);
                    vel[1] = 40.0f;
                    func_80352CF4(dust_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 6;
                } else if (btrot_walking) {
                    // Talon trot walking: alternating puffs (code_14420.c func_8029C22C)
                    f32 vel[3] = {0.0f, 40.0f, 0.0f};
                    f32 trot_pos[3];
                    f32 offset = (gm->dust_cooldown % 2 == 0) ? -20.0f : 20.0f;
                    func_802589E4(trot_pos, rs.yaw + offset, 20.0f);
                    trot_pos[0] += rs.x; trot_pos[1] = rs.y + 10.0f; trot_pos[2] += rs.z;
                    func_80352CF4(trot_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 6;
                } else if (start_moving || btrot_start) {
                    // Start walking/running: single puff
                    f32 vel[3] = {0.0f, 40.0f, 0.0f};
                    func_80352CF4(dust_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 10;
                }
            }
        }

        gm->prev_bs_state = rs.bs_state;
        gm->prev_y = rs.y;

        // Advance animation timer
        f32 dt = time_getDelta();
        f32 speed = dt / gm->anim_duration;
        gm->ghost_timer += speed;

        // === Per-frame phase transitions (height/timer based, mirrors game logic) ===

        // Jump height-based phase transitions (mirrors bsjump_update ground detection)
        if (gm->current_anim == ASSET_8_ANIM_BSJUMP && rs.bs_state == BS_5_JUMP) {
            f32 height = rs.y - gm->ground_y;
            bool descending = (rs.y < gm->prev_y - 1.0f);

            if (gm->anim_end < 0.51f && descending && height < 130.0f) {
                // Phase 0→2 direct: short jump (func_8028B254(0x82))
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->anim_duration = 1.4f;
            } else if (gm->anim_end > 0.5f && gm->anim_end < 0.7f && height < 90.0f) {
                // Phase 1→2: high jump approaching ground (func_8028B254(0x5A))
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->anim_duration = 2.0f;
            }
        }

        // Fall near-ground transition (mirrors bsjump_fall_update case 0)
        // When falling and near ground: switch from BSJUMP_FALL to BSJUMP landing pose
        // Only trigger when CROSSING the threshold (was above, now below) to avoid
        // false triggers after bpeck (player barely left the ground)
        if (gm->current_anim == ASSET_B0_ANIM_BSJUMP_FALL
            && (rs.bs_state == BS_2F_FALL || rs.bs_state == BS_20_LANDING)) {
            f32 height = rs.y - gm->ground_y;
            f32 prev_height = gm->prev_y - gm->ground_y;
            if (height < 90.0f && prev_height >= 90.0f) {
                // bsjump_fall_update: anctrl_setIndex BSJUMP, start 0.6667, dur 2.0, ONCE
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_8_ANIM_BSJUMP;
                gm->anim_duration = 2.0f;
                gm->anim_loops = FALSE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.6667f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
            }
        }

        // Bbuster per-frame phase transitions (mirrors bsbbuster_update)
        if (rs.bs_state == BS_F_BBUSTER && gm->current_anim == ASSET_1D_ANIM_BSBBUSTER) {
            f32 height = rs.y - gm->ground_y;
            f32 fall_speed = gm->prev_y - rs.y;
            bool on_ground = (height >= -10.0f && height < 30.0f);

            // Phase 0→1: wind-up done (timer reached 0.35), hold pose briefly
            if (gm->anim_end < 0.36f && gm->ghost_timer >= gm->anim_end - 0.002f) {
                gm->anim_duration = 0.4f;
                // Timer stays at 0.35, subRange unchanged — pose holds
            }

            // Phase 2→3: impact (on ground while still in wind-up subRange)
            // Extend subRange to 0.7299 for bounce+unroll animation, timer continues from ~0.35
            // No fall_speed check: network latency means char may already be bouncing up
            if (gm->anim_end < 0.36f && on_ground) {
                gm->anim_start = 0.0f;
                gm->anim_end = 0.7299f;
                gm->anim_duration = 1.9f;
                // ghost_timer stays at ~0.35, continues forward through bounce anim
            }

            // Phase 3→4: bounce animation done (timer reached 0.7299)
            // Hold at 0.74 with very slow duration until landing
            if (gm->anim_end > 0.72f && gm->anim_end < 0.74f
                && gm->ghost_timer >= gm->anim_end - 0.002f) {
                gm->anim_start = 0.0f;
                gm->anim_end = 0.74f;
                gm->anim_duration = 15.0f;
                // Near-freeze hold
            }

            // Phase 4→landing: on ground during recovery → play full landing
            if (gm->anim_end > 0.73f && gm->anim_end < 0.75f && on_ground) {
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->anim_duration = 1.9f;
                // Timer at ~0.74, plays rest of animation to landing
            }
        }

        // Bpeck per-frame phase transitions (mirrors bsbpeck_update phases 0-2)
        // Phase 0: BSBPECK(1A) ONCE 0.2f → at 91.26% switch to Phase 1
        // Phase 1: BSBPECK_ENTER(19) LOOP 0.35f for 0.5s → Phase 2
        // Phase 2: BSBPECK(1A) ONCE 0.2f (retract) → when done, BS changes from network
        if (rs.bs_state == BS_11_BPECK) {
            // Phase 0→1: at 91.26% of initial BSBPECK lunge
            // bpeck_timer == 0.0 means Phase 0 (initial). -1.0 means Phase 2 (retract).
            if (gm->current_anim == ASSET_1A_ANIM_BSBPECK
                && gm->bpeck_timer > -0.5f  // Phase 0 only (0.0), NOT Phase 2 (-1.0)
                && gm->ghost_timer >= 0.9126f) {
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_19_ANIM_BSBPECK_ENTER;
                gm->anim_duration = 0.35f;
                gm->anim_loops = TRUE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.0f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
                gm->bpeck_timer = 0.5f;
            }

            // Phase 1: countdown pecking loop, then → Phase 2 retract
            if (gm->current_anim == ASSET_19_ANIM_BSBPECK_ENTER && gm->bpeck_timer > 0.0f) {
                gm->bpeck_timer -= dt;
                if (gm->bpeck_timer <= 0.0f) {
                    ghost_prepare_blend(gm);
                    gm->prev_anim = gm->current_anim;
                    gm->current_anim = ASSET_1A_ANIM_BSBPECK;
                    gm->anim_duration = 0.2f;
                    gm->anim_loops = FALSE;
                    gm->anim_start = 0.0f;
                    gm->anim_end = 1.0f;
                    gm->ghost_timer = 0.0f;
                    gm->blend_timer = 0.0f;
                    gm->blending = TRUE;
                    gm->bpeck_timer = -1.0f;  // Mark as Phase 2 retract
                }
            }
        }

        // Idle kazooie peck→pull transition (0x20 entries in sequence)
        // At 37% of ASSET_95, switch to ASSET_F6 (Banjo pulls Kazooie back in)
        if ((rs.bs_state == BS_1_IDLE || rs.bs_state == BS_0_NONE)
            && gm->current_anim == ASSET_95_ANIM_BSSTAND_KAZOOIE_PECK
            && gm->ghost_timer >= 0.37f) {
            static const u8 idle_seq_check[21] = {
                0x09, 0x0A, 0x0C, 0x10, 0x09, 0x09, 0x0A, 0x09, 0x20,
                0x09, 0x0A, 0x0C, 0x10, 0x09, 0x09, 0x0A, 0x09, 0x10,
                0x09, 0x0A, 0x20
            };
            if (idle_seq_check[gm->idle_phase] & 0x20) {
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_F6_ANIM_BSSTAND_PULL_KAZOOIE;
                gm->anim_duration = 5.0f;
                gm->anim_loops = FALSE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.0f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
            }
        }

        // Bflip per-frame checks (mirrors bsbflip_update phases 0-4)
        if (rs.bs_state == BS_12_BFLIP) {
            f32 height = rs.y - gm->ground_y;
            f32 fall_speed = gm->prev_y - rs.y;  // positive = descending
            bool on_ground = (height >= -10.0f && height < 30.0f);

            // Phase 0→1: at 0.1837, speed up from 2.3f to 1.9f + launch
            if (gm->current_anim == ASSET_4B_ANIM_BSBFLIP_ENTER
                && gm->anim_end < 0.8f  // still in ENTER subRange phase
                && gm->ghost_timer >= 0.1837f
                && gm->anim_duration > 2.0f) {
                gm->anim_duration = 1.9f;
            }

            // Phase 2 (HOLD): A released detection → EXIT
            // Game: bakey_released(A) + baphysics_reset_terminal_velocity
            // Terminal velocity during hold = -533.3, after release = normal (~1400)
            // So fall speed increases significantly when A released
            if (gm->current_anim == ASSET_4C_ANIM_BSBFLIP_HOLD && fall_speed > 15.0f) {
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_61_ANIM_BSBFLIP_EXIT;
                gm->anim_duration = 0.8f;
                gm->anim_loops = FALSE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.0f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
            }

            // Phase 2 (HOLD): land directly while holding A (player_isStable, skips EXIT)
            if (gm->current_anim == ASSET_4C_ANIM_BSBFLIP_HOLD && on_ground) {
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_4B_ANIM_BSBFLIP_ENTER;
                gm->anim_duration = 2.2f;
                gm->anim_loops = FALSE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.8566f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
            }

            // Phase 3 (EXIT): when on ground → landing animation (ENTER at 0.8566)
            if (gm->current_anim == ASSET_61_ANIM_BSBFLIP_EXIT && on_ground) {
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_4B_ANIM_BSBFLIP_ENTER;
                gm->anim_duration = 2.2f;
                gm->anim_loops = FALSE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.8566f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
            }
        }

        // Bflap per-frame checks (mirrors bsbflap_update)
        if (rs.bs_state == BS_BFLAP) {
            f32 fall_speed = gm->prev_y - rs.y;  // positive = descending
            gm->bflap_elapsed += dt;

            // Detect A released: if falling faster than terminal velocity with flap gravity
            // With flap: gravity -1100, terminal -399.9 → fall speed ~400/frame_time
            // Without flap: normal gravity → much faster fall
            // Heuristic: if falling > 15 units/frame consistently, A was released
            if (!gm->bflap_released && gm->bflap_elapsed > 0.4f && fall_speed > 15.0f) {
                gm->bflap_released = TRUE;
                // A released: animation slows to 1.0f (from bsbflap_update phase 2/3)
                if (gm->current_anim == ASSET_17_ANIM_BSBFLAP) {
                    gm->anim_duration = 1.0f;
                }
            }

            // ENTER→loop transition: at 90% of ENTER animation (mirrors anctrl_isAt 0.9)
            if (gm->current_anim == ASSET_18_ANIM_BSBFLAP_ENTER && gm->ghost_timer >= 0.9f) {
                ghost_prepare_blend(gm);
                gm->prev_anim = gm->current_anim;
                gm->current_anim = ASSET_17_ANIM_BSBFLAP;
                gm->anim_loops = TRUE;
                gm->anim_start = 0.0f;
                gm->anim_end = 1.0f;
                gm->ghost_timer = 0.0f;
                gm->blend_timer = 0.0f;
                gm->blending = TRUE;
                // Duration from flap count (func_802A2858)
                gm->anim_duration = 0.15f;
                gm->bflap_count = 0;
            }

            // Flap counter: increment when loop animation reaches 90% (mirrors func_802A2810)
            // Then update duration (mirrors func_802A28CC → func_802A2858)
            // Note: timer can overshoot past 1.0 before wrap, so check if timer
            // is in 0.9-1.1 range (accounts for overshoot before clamp)
            if (gm->current_anim == ASSET_17_ANIM_BSBFLAP && !gm->bflap_released) {
                f32 advance = dt / gm->anim_duration;
                f32 prev_timer = gm->ghost_timer - advance;
                // Detect 0.9 crossing: either normal (prev<0.9, cur>=0.9)
                // or after wrap (prev was >0.9 last cycle, wrapped, now < prev)
                bool crossed = (prev_timer < 0.9f && gm->ghost_timer >= 0.9f)
                             || (gm->ghost_timer < prev_timer && prev_timer < 1.0f && prev_timer >= 0.9f);
                if (crossed) {
                    gm->bflap_count++;
                    // Update duration based on flap count (from func_802A2858)
                    switch (gm->bflap_count) {
                        case 0: gm->anim_duration = 0.15f; break;
                        case 1: gm->anim_duration = 0.2f; break;
                        case 2: gm->anim_duration = 0.27f; break;
                        case 3: gm->anim_duration = 0.38f; break;
                        default: gm->anim_duration = 0.4f; break;
                    }
                }
            }
        }

        // Clamp/loop within subRange
        if (gm->anim_loops) {
            if (gm->ghost_timer >= gm->anim_end) {
                gm->ghost_timer = gm->anim_start;
            }
        } else {
            if (gm->ghost_timer >= gm->anim_end) {
                // Jump timer-based phase transitions
                if (gm->current_anim == ASSET_8_ANIM_BSJUMP && gm->anim_end < 0.51f) {
                    // Phase 0→1: high jump, timer reached 0.5042
                    gm->anim_start = 0.0f;
                    gm->anim_end = 0.6667f;
                    gm->anim_duration = 4.0f;
                } else if (gm->current_anim == ASSET_8_ANIM_BSJUMP && gm->anim_end < 0.7f) {
                    // Phase 1→2: timer fallback
                    gm->anim_start = 0.0f;
                    gm->anim_end = 1.0f;
                    gm->anim_duration = 2.0f;
                }
                // Bflip ENTER done → switch to HOLD loop
                else if (gm->current_anim == ASSET_4B_ANIM_BSBFLIP_ENTER
                         && rs.bs_state == BS_12_BFLIP
                         && gm->anim_end < 0.8f) {
                    // Phase 1→2: ENTER finished → HOLD spin loop
                    {
                        ghost_prepare_blend(gm);
                        gm->prev_anim = gm->current_anim;
                        gm->current_anim = ASSET_4C_ANIM_BSBFLIP_HOLD;
                        gm->anim_duration = 0.13f;
                        gm->anim_loops = TRUE;
                        gm->anim_start = 0.0f;
                        gm->anim_end = 1.0f;
                        gm->ghost_timer = 0.0f;
                        gm->blend_timer = 0.0f;
                        gm->blending = TRUE;
                    }
                }
                // Idle cycle: when current idle anim finishes, advance phase
                // Sequence table from bsstand_update D_80364D20[21]:
                // 0x8|x = idle loop (ASSET_6F), 0x10 = kazooie peck (ASSET_95),
                // 0x20 = kazooie peck→pull (ASSET_95 → ASSET_F6 at 0.37)
                else if ((rs.bs_state == BS_1_IDLE || rs.bs_state == BS_0_NONE)
                         && (gm->current_anim == ASSET_6F_ANIM_BSSTAND_IDLE
                             || gm->current_anim == ASSET_95_ANIM_BSSTAND_KAZOOIE_PECK
                             || gm->current_anim == ASSET_F6_ANIM_BSSTAND_PULL_KAZOOIE)) {
                    static const u8 idle_seq[21] = {
                        0x09, 0x0A, 0x0C, 0x10, 0x09, 0x09, 0x0A, 0x09, 0x20,
                        0x09, 0x0A, 0x0C, 0x10, 0x09, 0x09, 0x0A, 0x09, 0x10,
                        0x09, 0x0A, 0x20
                    };
                    gm->idle_phase++;
                    if (gm->idle_phase > 20) gm->idle_phase = 0;
                    u8 phase = idle_seq[gm->idle_phase];

                    ghost_prepare_blend(gm);
                    gm->prev_anim = gm->current_anim;
                    gm->blend_timer = 0.0f;
                    gm->blending = TRUE;

                    if (phase & 0x10 || phase & 0x20) {
                        // Kazooie peck animation
                        gm->current_anim = ASSET_95_ANIM_BSSTAND_KAZOOIE_PECK;
                        gm->anim_duration = 5.5f;
                        gm->anim_loops = FALSE;
                    } else {
                        // Normal idle cycle — ONCE per 5.5s cycle, then advances counter
                        // (game uses anctrl_isAt(0.9999) to advance, we use timer reaching end)
                        gm->current_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
                        gm->anim_duration = 5.5f;
                        gm->anim_loops = FALSE;  // ONCE! Must finish to advance phase
                    }
                    gm->anim_start = 0.0f;
                    gm->anim_end = 1.0f;
                    gm->ghost_timer = 0.0f;
                } else {
                    // ONCE finished, no phase transition: hold last frame.
                    // Landing arms-open is handled by BS_20_LANDING force-transition.
                    gm->ghost_timer = gm->anim_end - 0.001f;
                }
            }
        }

        // Kazooie visibility — calculated AFTER all phase transitions and timer clamp.
        // Based on EXACT game source: grep of func_8029E070/064/058 across all bs/ files.
        // Head (D_8037D238): most K-active states + idle peeks
        // Wings (D_8037D236): ONLY bTrot, bLongLeg, bShock
        // Feet (D_8037D235): ONLY bEggAss
        {
            static const u8 idle_seq_vis[21] = {
                0x09, 0x0A, 0x0C, 0x10, 0x09, 0x09, 0x0A, 0x09, 0x20,
                0x09, 0x0A, 0x0C, 0x10, 0x09, 0x09, 0x0A, 0x09, 0x10,
                0x09, 0x0A, 0x20
            };
            bool k_head = FALSE;
            bool k_wings = FALSE;
            bool k_feet = FALSE;

            switch (rs.bs_state) {
                // Head ON (func_8029E070(1) in init)
                case BS_BFLAP: case BS_11_BPECK: case BS_12_BFLIP:
                case BS_F_BBUSTER: case BS_BBARGE:
                case BS_9_EGG_HEAD:
                case BS_23_FLY_ENTER: case BS_24_FLY: case BS_BOMB:
                case BS_18_FLY_KNOCKBACK: case BS_FLY_OW: case BS_58_BEAKBOMB_CRASH:
                case BS_41_DIE: case BS_D_TIMEOUT: case BS_53_TIMEOUT:
                case BS_44_JIG_JIGGY:
                case BS_1A_WONDERWING_ENTER: case BS_1B_WONDERWING_IDLE:
                case BS_1C_WONDERWING_WALK: case BS_1D_WONDERWING_JUMP:
                case BS_1E_WONDERWING_EXIT:
                    k_head = TRUE;
                    break;

                // Head + Wings ON (func_8029E070(1) + func_8029E064(1))
                case BS_14_BTROT_ENTER: case BS_15_BTROT_IDLE:
                case BS_16_BTROT_WALK: case BS_17_BTROT_EXIT:
                case BS_8_BTROT_JUMP:
                case BS_25_LONGLEG_ENTER: case BS_26_LONGLEG_IDLE:
                case BS_LONGLEG_WALK: case BS_LONGLEG_JUMP: case BS_LONGLEG_EXIT:
                    k_head = TRUE;
                    k_wings = TRUE;
                    break;

                // Head + Feet ON (func_8029E070(1) + func_8029E058(1))
                case BS_A_EGG_ASS:
                    k_head = TRUE;
                    k_feet = TRUE;
                    break;

                // Idle: head only during peek windows
                case BS_0_NONE: case BS_1_IDLE:
                    if (gm->current_anim == ASSET_95_ANIM_BSSTAND_KAZOOIE_PECK
                        || gm->current_anim == ASSET_F6_ANIM_BSSTAND_PULL_KAZOOIE) {
                        k_head = TRUE;
                    } else if (gm->current_anim == ASSET_6F_ANIM_BSSTAND_IDLE) {
                        u8 phase_type = idle_seq_vis[gm->idle_phase];
                        if (phase_type & 0x04) {
                            k_head = (gm->ghost_timer >= 0.0909f && gm->ghost_timer < 0.6818f);
                        } else if (phase_type & 0x02) {
                            k_head = (gm->ghost_timer >= 0.7727f);
                        }
                    }
                    break;

                // Talk/rest: head on
                case BS_3C_TALK:
                    k_head = TRUE;
                    break;

                // Everything else: all K hidden
                default:
                    break;
            }
            gm->kazooie_visible = k_head;
            gm->kazooie_body = k_wings;  // reuse for wings
            gm->kazooie_feet = k_feet;
        }

        // Load current animation frame into bone_current
        // Done AFTER all phase transitions and clamp/loop so bone_current
        // always reflects the final animation state for this frame.
        {
            void *anim_file = animBinCache_get(gm->current_anim);
            if (anim_file) {
                animationFile_getBoneTransformList(anim_file, gm->ghost_timer, gm->bone_current);
            }
        }


        // Determine which bone list to use for rendering
        void *render_bones = gm->bone_current;

        if (gm->blending) {
            gm->blend_timer += dt;
            f32 blend_t = gm->blend_timer / BLEND_DURATION;
            if (blend_t >= 1.0f) {
                blend_t = 1.0f;
                gm->blending = FALSE;
            }
            boneTransformList_interpolate(gm->bone_blend, gm->bone_prev, gm->bone_current, blend_t);
            render_bones = gm->bone_blend;
        }

        // Yaw (faster during skid)
        f32 yaw_speed = (rs.bs_state == BS_SKID) ? 0.7f : 0.25f;

        // Ground tracking
        bool on_ground = (rs.bs_state == BS_1_IDLE || rs.bs_state == BS_0_NONE
            || rs.bs_state == BS_WALK || rs.bs_state == BS_2_WALK_SLOW
            || rs.bs_state == BS_4_WALK_FAST || rs.bs_state == BS_WALK_CREEP
            || rs.bs_state == BS_CROUCH || rs.bs_state == BS_CLAW
            || rs.bs_state == BS_SKID || rs.bs_state == BS_ROLL
            || rs.bs_state == BS_15_BTROT_IDLE || rs.bs_state == BS_16_BTROT_WALK
            || rs.bs_state == BS_20_LANDING);
        if (on_ground) gm->ground_y = rs.y;

        gm->smooth_yaw = lerp_angle(gm->smooth_yaw, rs.yaw, yaw_speed);
        f32 pos[3] = {rs.x, rs.y, rs.z};
        f32 rot[3] = {rs.pitch, gm->smooth_yaw, 0.0f};
        f32 ref[3] = {0.0f, 0.0f, 0.0f};

        cur_drawn_model_transform_id = GHOST_TRANSFORM_ID_START + (pid * GHOST_TRANSFORM_ID_STRIDE);

        // Shadow on ground
        if (gm->shadow_model) {
            f32 height = rs.y - gm->ground_y;
            if (height < 0.0f) height = 0.0f;
            f32 shadow_scale = 0.43f - (height / 800.0f);
            if (shadow_scale < 0.15f) shadow_scale = 0.15f;
            f32 sp[3] = {rs.x, gm->ground_y + 4.0f, rs.z};
            f32 sr[3] = {0.0f, 0.0f, 0.0f};
            modelRender_setAlpha(0xFF);
            modelRender_setDepthMode(MODEL_RENDER_DEPTH_COMPARE);
            modelRender_draw(gfx, mtx, sp, sr, shadow_scale, 0, gm->shadow_model);
        }

        // Use the AnimCtrl's bone buffer (correct structure for renderer).
        // 1. Call baanim_80289F30 to set up render state with local bones
        // 2. Get the AnimCtrl's bone buffer pointer
        // 3. Load ghost animation INTO that buffer (overwrite local data)
        // 4. Render ghost
        // baanim_80289F30 at end of loop restores local data in the buffer.
        baanim_80289F30();
        func_8029DD6C();

        // Use AnimCtrl's bone buffer (correct structure for renderer).
        // 1. baanim_80289F30 sets up render state with local bones
        // 2. func_8029DD6C sets nodes correctly
        // 3. Load ghost animation INTO the AnimCtrl's buffer
        // 4. Render ghost with correct bone structure
        // 5. baanim_80289F30 at end of loop restores local data
        baanim_80289F30();
        func_8029DD6C();

        {
            AnimCtrl *ac = baanim_getAnimCtrlPtr();
            Animation *anim_ptr = anctrl_getAnimPtr(ac);
            void *bone_buffer = animcache_getCurrentTransform(anim_ptr);
            if (bone_buffer) {
                void *anim_file = animBinCache_get(gm->current_anim);
                if (anim_file) {
                    animationFile_getBoneTransformList(anim_file, gm->ghost_timer, bone_buffer);
                }

                if (gm->blending) {
                    void *prev_file = animBinCache_get(gm->prev_anim);
                    if (prev_file && gm->bone_prev) {
                        animationFile_getBoneTransformList(prev_file, 0.0f, gm->bone_prev);
                        f32 blend_t = gm->blend_timer / BLEND_DURATION;
                        if (blend_t > 1.0f) blend_t = 1.0f;
                        boneTransformList_interpolate(bone_buffer, gm->bone_prev, bone_buffer, blend_t);
                    }
                }
            }
        }

        {
            s32 env_color[3];
            func_8029A47C(env_color);
            modelRender_setEnvColor(env_color[0], env_color[1], env_color[2], 255);
        }
        func_8033A280(2.0f);
        func_8033A450(D_80363780);
        modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
        ghost_setup_model_nodes(gm->kazooie_visible, gm->kazooie_body, gm->kazooie_feet);
        bkrecomp_setup_custom_skinning(&ghost_skinning_data[pid], baModel_getModelId());

        modelRender_draw(gfx, mtx, pos, rot, baModelScale, ref, baModelBin);
    }

    // Restore local player state after all ghost renders:
    // 1. baanim_80289F30 restores modelRenderBoneTransformList to local player's bones
    // 2. func_8033A444(NULL) forces D_8038371C rebuild on next frame from local bones
    //    (without this, local player would use ghost's AnimMtxList on next frame)
    baanim_80289F30();
    func_8033A444((void*)0);
}

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {}
RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized (blended anims)\n");
}
