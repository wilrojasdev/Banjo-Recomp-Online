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

// Must match NetFullState layout in network_patches.c and net_recomp_api.cpp
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
extern void animationFile_getBoneTransformList(void *anim_file, f32 progress, void *bone_list);
extern void *boneTransformList_new(void);
extern void modelRender_setBoneTransformList(void *bone_list);
extern void boneTransformList_interpolate(void *result, void *start, void *end, f32 t);
extern f32 time_getDelta(void);
extern f32 mapModel_getFloorY(f32 pos[3]);

// Dust particles
extern void func_80352CF4(f32 pos[3], f32 vel[3], f32 startScale, f32 endScale);
extern void func_802589E4(f32 dst[3], f32 angle, f32 magnitude);
extern f32 mlNormalizeAngle(f32 angle);

#define MAX_PLAYERS 4
#define BLEND_DURATION 0.15f

typedef struct {
    void *shadow_model;
    void *bone_current;   // Ghost's own bone buffer
    void *bone_prev;      // Previous pose for blending
    void *bone_render;    // Blended result for rendering
    u16 current_anim;
    u16 prev_anim;
    f32 smooth_yaw;
    f32 ghost_timer;
    f32 blend_timer;
    f32 ground_y;
    u8 prev_bs_state;
    u8 dust_cooldown;
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

extern void func_8033A1FC(void);
// Getter for D_80383658[index] — model node display list value
extern s32 func_8033A0F0(s32 index);

// Full model node setup for ghost — replaces func_8029DD6C entirely.
// We can't rely on func_8029DD6C because it uses baModel_getModelId()
// and global eye state from the LOCAL player. The ghost always renders
// as BK model and needs its own node configuration.
// Mirrors func_8029DD6C + func_8029DBF0 for ASSET_34D/34E (BK model).
static void ghost_setup_all_model_nodes(bool kazooie_head, bool kazooie_wings, bool kazooie_feet) {
    // Clear all 42 nodes
    func_8033A1FC();

    // Kazooie head/beak (D_8037D238 equivalent)
    s32 k_h = kazooie_head ? 1 : 0;
    func_8033A45C(1, k_h);
    func_8033A45C(9, k_h);
    func_8033A45C(0xC, k_h);
    func_8033A45C(0xF, k_h);

    // Kazooie wings (D_8037D236 equivalent)
    s32 k_w = kazooie_wings ? 1 : 0;
    func_8033A45C(2, k_w);
    func_8033A45C(0xA, k_w);
    func_8033A45C(0xD, k_w);
    func_8033A45C(0x10, k_w);

    // Kazooie feet (D_8037D235 equivalent)
    s32 k_f = kazooie_feet ? 1 : 0;
    func_8033A45C(8, k_f);
    func_8033A45C(0xB, k_f);
    func_8033A45C(0xE, k_f);
    func_8033A45C(0x11, k_f);

    // Tails (D_8037D237 equivalent): visible (value = 0 + 1 = 1)
    func_8033A45C(0x12, 1);
    func_8033A45C(0x14, 1);
    func_8033A45C(0x16, 1);
    func_8033A45C(0x18, 1);
    func_8033A45C(0x13, 1);
    func_8033A45C(0x15, 1);
    func_8033A45C(0x17, 1);
    func_8033A45C(0x19, 1);

    // Eyes (from func_8029DBF0): always open for ghost (value = 1)
    // D_8037D23C=0.0 → ml_interpolate_f(0.0,1.0,8.0) = 1
    // D_8037D240=0.0 → ml_interpolate_f(0.0,1.0,8.0) = 1
    func_8033A45C(0x1B, 1);
    func_8033A45C(0x1D, 1);
    func_8033A45C(0x1F, 1);
    func_8033A45C(0x21, 1);
    func_8033A45C(0x1A, 1);
    func_8033A45C(0x1C, 1);
    func_8033A45C(0x1E, 1);
    func_8033A45C(0x20, 1);

    // Body parts (D_8037D239 equivalent): visible (value = 0 + 1 = 1)
    func_8033A45C(0x22, 1);
    func_8033A45C(0x24, 1);
    func_8033A45C(0x26, 1);
    func_8033A45C(0x28, 1);
    func_8033A45C(0x23, 1);
    func_8033A45C(0x25, 1);
    func_8033A45C(0x27, 1);
    func_8033A45C(0x29, 1);
}

// Capture current pose into bone_prev for blending
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
    gm->bone_render = boneTransformList_new();
    if (!gm->bone_current || !gm->bone_prev || !gm->bone_render) return;

    void *anim_file = animBinCache_get(ASSET_6F_ANIM_BSSTAND_IDLE);
    if (anim_file) {
        animationFile_getBoneTransformList(anim_file, 0.0f, gm->bone_current);
        animationFile_getBoneTransformList(anim_file, 0.0f, gm->bone_prev);
    }

    gm->current_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->prev_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->smooth_yaw = 0.0f;
    gm->ghost_timer = 0.0f;
    gm->ground_y = 0.0f;
    gm->blend_timer = BLEND_DURATION;
    gm->blending = FALSE;
    gm->initialized = TRUE;
}

void bkrecomp_net_draw_ghosts(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    if (!recomp_net_is_connected()) return;
    if (!baModelBin) return;

    u32 local_id = recomp_net_get_local_player_id();
    u32 local_map = (u32)map_get();

    // Snapshot ALL local player render state BEFORE any ghost rendering.
    s32 saved_nodes[0x2A];
    s32 i;
    for (i = 0; i < 0x2A; i++) saved_nodes[i] = func_8033A0F0(i);

    for (u32 pid = 0; pid < MAX_PLAYERS; pid++) {
        if (pid == local_id) continue;

        RemoteState rs;
        if (!recomp_net_get_remote_state(pid, &rs)) continue;
        if (rs.map_id != local_map || rs.x < -15000.0f) continue;

        ghost_ensure_init(pid);
        GhostModel *gm = &ghost_models[pid];
        if (!gm->initialized) continue;

        // === Direct animation mirror from local player's AnimCtrl ===
        if (rs.animation_id != gm->current_anim) {
            recomp_printf("[Ghost%d] Anim change: 0x%03X → 0x%03X timer=%.3f bs=0x%02X\n",
                pid, gm->current_anim, rs.animation_id, rs.anim_timer, rs.bs_state);
            ghost_prepare_blend(gm);
            gm->prev_anim = gm->current_anim;
            gm->current_anim = rs.animation_id;
            gm->blend_timer = 0.0f;
            gm->blending = TRUE;
        }
        gm->ghost_timer = rs.anim_timer;

        // Kazooie visibility from packed flags
        bool kazooie_head  = (rs.kazooie_flags & 1) != 0;
        bool kazooie_wings = (rs.kazooie_flags & 2) != 0;
        bool kazooie_feet  = (rs.kazooie_flags & 4) != 0;

        // === Dust particles ===
        {
            f32 ghost_pos[3] = {rs.x, rs.y, rs.z};
            f32 floor_y = mapModel_getFloorY(ghost_pos);
            f32 height = rs.y - floor_y;
            bool on_ground = (height >= -10.0f && height < 30.0f);
            if (on_ground) gm->ground_y = floor_y;

            if (gm->dust_cooldown > 0) gm->dust_cooldown--;

            bool running = (rs.bs_state == BS_4_WALK_FAST || rs.bs_state == BS_WALK);
            bool walking = (rs.bs_state == BS_2_WALK_SLOW || rs.bs_state == BS_WALK_CREEP);

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
                    f32 vel[3] = {0.0f, 40.0f, 0.0f};
                    f32 barge_pos[3];
                    func_802589E4(barge_pos, rs.yaw - 20.0f, 20.0f);
                    barge_pos[0] += rs.x; barge_pos[1] = rs.y + 10.0f; barge_pos[2] += rs.z;
                    func_80352CF4(barge_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 4;
                } else if (direction_change) {
                    f32 vel[3];
                    func_802589E4(vel, rs.yaw, 200.0f * 0.51f);
                    vel[1] = 40.0f;
                    func_80352CF4(dust_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 6;
                } else if (btrot_walking) {
                    f32 vel[3] = {0.0f, 40.0f, 0.0f};
                    f32 trot_pos[3];
                    f32 offset = (gm->dust_cooldown % 2 == 0) ? -20.0f : 20.0f;
                    func_802589E4(trot_pos, rs.yaw + offset, 20.0f);
                    trot_pos[0] += rs.x; trot_pos[1] = rs.y + 10.0f; trot_pos[2] += rs.z;
                    func_80352CF4(trot_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 6;
                } else if (start_moving || btrot_start) {
                    f32 vel[3] = {0.0f, 40.0f, 0.0f};
                    func_80352CF4(dust_pos, vel, 10.0f, 150.0f);
                    gm->dust_cooldown = 10;
                }
            }
        }

        gm->prev_bs_state = rs.bs_state;

        // === Load bone_current from received animation state ===
        {
            void *anim_file = animBinCache_get(gm->current_anim);
            if (anim_file) {
                animationFile_getBoneTransformList(anim_file, gm->ghost_timer, gm->bone_current);
            } else {
                recomp_printf("[Ghost%d] WARNING: animBinCache_get(0x%03X) returned NULL!\n",
                    pid, gm->current_anim);
            }
        }

        // Advance blend timer
        f32 dt = time_getDelta();
        if (gm->blending) {
            gm->blend_timer += dt;
            if (gm->blend_timer >= BLEND_DURATION) {
                gm->blending = FALSE;
            }
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

        // Shadow
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

        // === Ghost render using AnimCtrl's bone buffer ===
        // Use the AnimCtrl's bone buffer (correct format/size for BK model).
        // Save local bones, write ghost bones, render, restore.
        baanim_80289F30();
        func_8029DD6C();

        {
            AnimCtrl *ac = baanim_getAnimCtrlPtr();
            Animation *anim_ptr = anctrl_getAnimPtr(ac);
            void *bone_buffer = animcache_getCurrentTransform(anim_ptr);
            if (bone_buffer) {
                // Save local bones into ghost's bone_render (used as temp save buffer)
                boneTransformList_interpolate(gm->bone_render, bone_buffer, bone_buffer, 0.0f);

                // Write ghost bones into the AnimCtrl's buffer
                if (gm->blending) {
                    f32 blend_t = gm->blend_timer / BLEND_DURATION;
                    if (blend_t > 1.0f) blend_t = 1.0f;
                    boneTransformList_interpolate(bone_buffer, gm->bone_prev, gm->bone_current, blend_t);
                } else {
                    void *anim_file = animBinCache_get(gm->current_anim);
                    if (anim_file) {
                        animationFile_getBoneTransformList(anim_file, gm->ghost_timer, bone_buffer);
                    }
                }

                // Force matrix recompute from ghost bones
                func_8033A444((void*)0);
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
        ghost_setup_all_model_nodes(kazooie_head, kazooie_wings, kazooie_feet);
        bkrecomp_setup_custom_skinning(&ghost_skinning_data[pid], baModel_getModelId());

        modelRender_draw(gfx, mtx, pos, rot, baModelScale, ref, baModelBin);

        // Restore local bones immediately after ghost render
        {
            AnimCtrl *ac = baanim_getAnimCtrlPtr();
            Animation *anim_ptr = anctrl_getAnimPtr(ac);
            void *bone_buffer = animcache_getCurrentTransform(anim_ptr);
            if (bone_buffer) {
                boneTransformList_interpolate(bone_buffer, gm->bone_render, gm->bone_render, 0.0f);
            }
        }
    }

    // === Restore local player render state ===
    // Model nodes: exact snapshot restore
    for (i = 0; i < 0x2A; i++) func_8033A45C(i, saved_nodes[i]);
    // Bones + matrices: baanim_80289F30 sets bone pointer and matrices from
    // the (now restored) local bone data. Do NOT clear AnimMtxList after.
    baanim_80289F30();
}

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {}
RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized (direct mirror)\n");
}
