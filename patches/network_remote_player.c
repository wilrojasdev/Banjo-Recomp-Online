#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/modelRender.h"
#include "core2/anctrl.h"
#include "core2/commonParticle.h"
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
} RemoteState;

extern void *baModelBin;
extern f32 baModelScale;
extern void func_8029A47C(s32 env_color[3]);
extern void func_8033A280(f32);
extern struct5Bs *D_80363780;
extern void func_8033A450(struct5Bs *);
extern void baanim_80289F30(void);

extern void *animBinCache_get(enum asset_e asset_id);
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
    f32 blend_timer;      // 0.0 = fully prev, BLEND_DURATION = fully current
    f32 ground_y;
    u8 prev_bs_state;
    u8 dust_cooldown;
    bool anim_loops;
    bool blending;
    bool initialized;
} GhostModel;

static GhostModel ghost_models[MAX_PLAYERS] = {0};

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
    gm->ground_y = 0.0f;
    gm->blend_timer = BLEND_DURATION;
    gm->blending = FALSE;
    gm->anim_loops = TRUE;
    gm->initialized = TRUE;
}

static void ghost_sync_anim(GhostModel *gm, u8 bs_state) {
    if (!gm->initialized) return;

    u16 anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    f32 duration = 5.5f;
    bool loops = TRUE;

    switch (bs_state) {
        case BS_0_NONE: case BS_1_IDLE:
            anim = ASSET_6F_ANIM_BSSTAND_IDLE; duration = 5.5f; loops = TRUE; break;
        case BS_20_LANDING:
            // Keep previous animation playing (e.g. bbuster continues during landing)
            return;
        case BS_D_TIMEOUT: case BS_53_TIMEOUT:
            anim = ASSET_77_ANIM_BSTIMEOUT; duration = 3.2f; loops = TRUE; break;
        case BS_WALK_CREEP:
            anim = ASSET_2_ANIM_BSWALK_CREEP; duration = 1.5f; break;
        case BS_2_WALK_SLOW:
            anim = ASSET_3_ANIM_BSWALK; duration = 1.5f; break;
        case BS_WALK:  anim = ASSET_C_ANIM_BSWALK_RUN; duration = 1.5f; break;
        case BS_4_WALK_FAST: anim = ASSET_C_ANIM_BSWALK_RUN; duration = 0.6f; break;
        case BS_SKID:  anim = ASSET_E_ANIM_BSTURN; duration = 0.3f; loops = TRUE; break;
        case BS_SLIDE: anim = ASSET_5A_ANIM_BSSLIDE_FRONT; duration = 1.0f; break;
        case BS_ROLL:  anim = ASSET_4F_ANIM_BSTWIRL; duration = 0.9f; loops = TRUE; break;
        case BS_5_JUMP: anim = ASSET_8_ANIM_BSJUMP; duration = 1.9f; loops = FALSE; break;
        case BS_12_BFLIP: anim = ASSET_4B_ANIM_BSBFLIP_ENTER; duration = 2.3f; loops = FALSE; break;
        case BS_2F_FALL: anim = ASSET_8_ANIM_BSJUMP; duration = 2.0f; loops = FALSE; break;
        case BS_3D_FALL_TUMBLING: anim = ASSET_68_ANIM_BSJUMP_TUMBLE; duration = 0.35f; loops = TRUE; break;
        case BS_CLAW:  anim = ASSET_5_ANIM_BSPUNCH; duration = 1.3f; loops = FALSE; break;
        case BS_F_BBUSTER: anim = ASSET_1D_ANIM_BSBBUSTER; duration = 1.9f; loops = FALSE; break;
        case BS_BFLAP:
            if (gm->current_anim != ASSET_18_ANIM_BSBFLAP_ENTER && gm->current_anim != ASSET_17_ANIM_BSBFLAP) {
                anim = ASSET_18_ANIM_BSBFLAP_ENTER; duration = 0.30f; loops = FALSE;
            } else if (gm->current_anim == ASSET_18_ANIM_BSBFLAP_ENTER && gm->ghost_timer >= 0.95f) {
                anim = ASSET_17_ANIM_BSBFLAP; duration = 0.15f; loops = TRUE;
            } else {
                return;
            }
            break;
        case BS_11_BPECK: anim = ASSET_19_ANIM_BSBPECK_ENTER; duration = 0.35f; loops = TRUE; break;
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

    if (anim != gm->current_anim) {
        // recomp_printf("[GhostAnim] BS=0x%02X anim=0x%03X dur=%.2f %s\n", bs_state, anim, duration, loops ? "LOOP" : "ONCE");

        // Copy current bones to prev for blending
        // (bone_current has the last frame of the old animation)
        void *tmp = gm->bone_prev;
        gm->bone_prev = gm->bone_current;
        gm->bone_current = tmp;

        gm->prev_anim = gm->current_anim;
        gm->current_anim = anim;
        gm->anim_duration = duration;
        gm->anim_loops = loops;
        gm->blend_timer = 0.0f;
        gm->blending = TRUE;

        if (anim == ASSET_8_ANIM_BSJUMP && bs_state == BS_5_JUMP) {
            gm->ghost_timer = 0.35f; // Skip crouch windup for jump
        } else if (anim == ASSET_8_ANIM_BSJUMP && bs_state == BS_2F_FALL) {
            gm->ghost_timer = 0.7f;  // Start at falling pose for fall
        } else if (anim == ASSET_E_ANIM_BSTURN) {
            gm->ghost_timer = 0.4f;  // Skip arms-open start frame
        } else {
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

        ghost_sync_anim(gm, rs.bs_state);

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

        // Load current animation frame into bone_current
        void *anim_file = animBinCache_get(gm->current_anim);
        if (anim_file) {
            animationFile_getBoneTransformList(anim_file, gm->ghost_timer, gm->bone_current);
        }

        // Advance animation timer
        f32 dt = time_getDelta();
        f32 speed = dt / gm->anim_duration;
        gm->ghost_timer += speed;
        if (gm->anim_loops) {
            if (gm->ghost_timer >= 1.0f) gm->ghost_timer -= 1.0f;
        } else {
            if (gm->ghost_timer >= 0.99f) gm->ghost_timer = 0.99f;
        }

        // Determine which bone list to use for rendering
        void *render_bones = gm->bone_current;

        if (gm->blending) {
            // Advance blend timer
            gm->blend_timer += dt;
            f32 blend_t = gm->blend_timer / BLEND_DURATION;
            if (blend_t >= 1.0f) {
                blend_t = 1.0f;
                gm->blending = FALSE;
            }
            // Interpolate: prev → current
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

        // Set blended bone transforms and render
        modelRender_setBoneTransformList(render_bones);

        s32 env_color[3];
        func_8029A47C(env_color);
        modelRender_setEnvColor(env_color[0], env_color[1], env_color[2], 255);
        func_8033A280(2.0f);
        func_8033A450(D_80363780);
        modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
        modelRender_draw(gfx, mtx, pos, rot, baModelScale, ref, baModelBin);
    }

    baanim_80289F30();
}

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {}
RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized (blended anims)\n");
}
