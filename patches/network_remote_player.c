#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "core2/modelRender.h"
#include "core2/commonParticle.h"

extern AnimMtxList *D_8038371C;

extern s32 commonParticle_new(enum common_particle_e particle_id, s32 arg1);
extern void commonParticle_add(s32 actorMarker, s32 arg1, s32 arg2);
#include "animation.h"
#include "transform_ids.h"

u32 recomp_net_is_connected(void);
u32 recomp_net_get_remote_state(u32 player_id, void* out);
u32 recomp_net_get_local_player_id(void);
void recomp_net_push_camera_state(void* cam_data);

extern enum map_e map_get(void);
extern s32 gFramebufferWidth;
extern s32 gFramebufferHeight;

// Viewport functions for camera state bridge
extern void viewport_getPosition_vec3f(f32 arg0[3]);
extern void viewport_getRotation_vec3f(f32 arg0[3]);
extern f32 viewport_getFOVy(void);
extern f32 viewport_getNear(void);
extern f32 viewport_getAspectRatio(void);
extern s32 cur_drawn_model_transform_id;

#define GHOST_TRANSFORM_ID_START  0x20000000
#define GHOST_TRANSFORM_ID_STRIDE 0x100

/* 1 = recomp_printf ghost SKID frame-by-frame lines. Rebuild patches after
 * changing. Pair this with the same flag in network_patches.c so sender and
 * receiver dumps line up. */
#ifndef BKRECOMP_NET_GHOST_ANIM_LOG
#define BKRECOMP_NET_GHOST_ANIM_LOG 1
#endif

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
    u8  carry_kind;     // 0x3C — visual prop replication (CARRY_KIND_*)
    u8  _pad3[3];
} RemoteState;

extern void *baModelBin;
extern f32 baModelScale;
extern void func_8029A47C(s32 env_color[3]);
extern void func_8033A280(f32);
extern struct5Bs *D_80363780;
extern void func_8033A450(struct5Bs *);
extern void modelRender_setBoneTransformList(void *bone_transform_list);
extern void baanim_80289F30(void);
extern void func_8029DD6C(void);
extern enum asset_e baModel_getModelId(void);
extern void bkrecomp_setup_custom_skinning(ModelSkinningData* skinning_data, u32 model_id);

extern void *animBinCache_get(enum asset_e asset_id);
extern void animationFile_getBoneTransformList(void *anim_file, f32 progress, void *bone_list);
extern void *boneTransformList_new(void);
extern void boneTransformList_reset(void *bone_list);
extern void boneTransformList_interpolate(void *result, void *start, void *end, f32 t);
extern f32 time_getDelta(void);
extern f32 mapModel_getFloorY(f32 pos[3]);

// Dust particles
extern void func_80352CF4(f32 pos[3], f32 vel[3], f32 startScale, f32 endScale);
extern void func_802589E4(f32 dst[3], f32 angle, f32 magnitude);
extern f32 mlNormalizeAngle(f32 angle);

// Transformation particles (from dronexform.c / particle system)
extern void partEmitMgr_freeEmitter(ParticleEmitter *this);
extern void func_802EFF50(ParticleEmitter *, f32);
extern f32 ml_interpolate_f(f32 t, f32 a, f32 b);
extern f32 ml_remainder_f(f32 a, f32 b);
extern f32 func_80257A44(f32 a, f32 b);

// Mumbo lock state (from network_flag_sync.c)
extern bool bkrecomp_net_mumbo_is_locked(void);
extern u8 bkrecomp_net_mumbo_get_lock_owner(void);

// commonParticle.h provides: commonParticle_new, projectile_setPosition, etc.
// Feather visual actor spawning (from code_51950.c)
extern void func_8032AA58(Actor *, f32);  // set actor lifetime/fade speed

// Asset cache (assetcache_get already in functions.h)
extern void assetcache_release(void *bin);

/* Mirrors CarryKind in src/net/net_packets.h. Add new kinds here when
 * networking other carryable quest items (bullion, jinjos, etc.). */
#define NET_CARRY_KIND_NONE   0
#define NET_CARRY_KIND_ORANGE 1

/* Cached model bins for carried-prop rendering on remote ghosts. Loaded
 * lazily on first use (one allocation per asset, lives for the session;
 * BK's asset cache is process-global so re-getting is a no-op anyway). */
static void *s_carry_orange_model_bin = NULL;

#define MAX_PLAYERS 4

typedef struct {
    void *shadow_model;
    void *bone_save;           // Final bones for modelRender + skinning output
    void *bone_blend_temp;     // Scratch: animationFile_getBoneTransformList target
    u16 current_anim;
    u16 bone_blend_base_anim;  // last anim for which bone_blend_temp was reset
    f32 smooth_yaw;
    f32 ghost_timer;
    f32 ground_y;
    f32 prev_y;           // Previous frame Y for landing detection
    u8 prev_bs_state;
    u8 dust_cooldown;
    bool bbuster_dust_done; // Prevent bbuster dust from firing twice (impact + bounce)
    bool egg_fired;       // Prevent multiple egg spawns per animation
    u8 feather_cooldown;  // Cooldown ticks between feather particle spawns
    bool initialized;
    // Transformation particle emitters
    ParticleEmitter *xform_emit_blue;
    ParticleEmitter *xform_emit_yellow;
    f32 xform_orbit_progress;  // 0..1 orbit rotation
    bool xform_particles_active;
    // Transformation model cache
    void *xform_model_bin;        // Loaded model binary (NULL = use baModelBin)
    u8    cached_transformation;  // Which transformation is currently cached
    enum asset_e cached_model_id; // Asset ID of the cached model (for skinning)
    s8 slide_dust_phase;            // BS_SLIDE dust side alternation (per peer, not static)
    /* Per-ghost world-space bone buffer. Set as the current bone-output
     * target via func_8033A450 right before this ghost's modelRender_draw
     * so the bones the renderer would otherwise write into the local
     * player's D_80363780 land here instead. Lets carry-prop renders
     * (e.g. the orange) anchor to the ghost's body center *with*
     * animation bob, not just rs.position + fixed offset. */
    struct5Bs *bones_world;
    /* Cross-fade state for multi-phase chained clips (BFLIP/BPECK/FALL).
     * On anim change while in those states, we snapshot the prior rendered
     * pose into bone_fade_from and lerp toward the freshly-sampled new
     * clip over fade_duration seconds. Replaces the older "carry stale
     * bones forward" hack which leaked unanimated bones across phase
     * boundaries and produced the twisted poses these moves showed. */
    void *bone_fade_from;
    f32   fade_progress;
    bool  fade_active;
} GhostModel;

extern struct5Bs *func_8034A2C8(void);
extern void func_8034A174(struct5Bs *this, s32 indx, f32 dst[3]);

static GhostModel ghost_models[MAX_PLAYERS] = {0};
static ModelSkinningData ghost_skinning_data[MAX_PLAYERS] = {0};

/* Tracks carry_kind from the previous frame per ghost so we can detect
 * 0->ORANGE / ORANGE->0 transitions and trigger world-actor side effects:
 *   - 0 -> ORANGE: despawn the tree-position orange actor on this peer
 *     (the carrier is now holding it; the local copy should disappear).
 *   - ORANGE -> 0: spawn an ORANGE_COLLECTIBLE at the carrier's position
 *     so the throw "lands" visibly here. Vanilla chLevelCollectible_update
 *     handles its lifetime — it self-despawns on FLAG_3_CHIMPY_HAS_LEAVED.
 * Reset to 0 implicitly when a slot is cleared. */
static u8 ghost_prev_carry_kind[MAX_PLAYERS] = {0};

extern Actor *actorArray_findActorFromMarkerId(s32 marker_id);
extern void marker_despawn(ActorMarker *marker);
extern Actor *actor_spawnWithYaw_s32(s32 actor_id, s32 position[3], s32 yaw);
/* nodeprop_findByActorIdAndActorPosition + nodeprop_getPosition come from
 * functions.h. */

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
// and global eye state from the LOCAL player.
// Mirrors func_8029DD6C + func_8029DBF0 for each model type.
static void ghost_setup_all_model_nodes(u8 transformation, bool kazooie_head, bool kazooie_wings, bool kazooie_feet) {
    // Clear all 42 nodes
    func_8033A1FC();

    switch (transformation) {
        case TRANSFORM_4_WALRUS:
            // Walrus model: node 3 = mouth (D_8037D23A equivalent, always visible)
            func_8033A45C(3, 1);
            // Eyes: nodes 0x1A, 0x1B (from func_8029DBF0, always open = 1)
            func_8033A45C(0x1A, 1);
            func_8033A45C(0x1B, 1);
            break;

        case TRANSFORM_5_CROC:
            // Croc model: nodes 4-7 = tail segments (D_8037D237 equivalent, visible)
            func_8033A45C(4, 1);
            func_8033A45C(5, 1);
            func_8033A45C(6, 1);
            func_8033A45C(7, 1);
            // Eyes: nodes 0x1A, 0x1B (from func_8029DBF0, always open = 1)
            func_8033A45C(0x1A, 1);
            func_8033A45C(0x1B, 1);
            break;

        case TRANSFORM_2_TERMITE:
        case TRANSFORM_3_PUMPKIN:
        case TRANSFORM_6_BEE:
            // Eyes: nodes 0x1A, 0x1B (from func_8029DBF0, always open = 1)
            func_8033A45C(0x1A, 1);
            func_8033A45C(0x1B, 1);
            break;

        case TRANSFORM_7_WISHWASHY:
            // WishyWashy eyes: node 1 (from func_8029DBF0, always open = 1)
            func_8033A45C(1, 1);
            break;

        default: {
            // Banjo-Kazooie model — full node setup
            s32 k_h = kazooie_head ? 1 : 0;
            func_8033A45C(1, k_h);
            func_8033A45C(9, k_h);
            func_8033A45C(0xC, k_h);
            func_8033A45C(0xF, k_h);

            s32 k_w = kazooie_wings ? 1 : 0;
            func_8033A45C(2, k_w);
            func_8033A45C(0xA, k_w);
            func_8033A45C(0xD, k_w);
            func_8033A45C(0x10, k_w);

            s32 k_f = kazooie_feet ? 1 : 0;
            func_8033A45C(8, k_f);
            func_8033A45C(0xB, k_f);
            func_8033A45C(0xE, k_f);
            func_8033A45C(0x11, k_f);

            // Tails: visible
            func_8033A45C(0x12, 1);
            func_8033A45C(0x14, 1);
            func_8033A45C(0x16, 1);
            func_8033A45C(0x18, 1);
            func_8033A45C(0x13, 1);
            func_8033A45C(0x15, 1);
            func_8033A45C(0x17, 1);
            func_8033A45C(0x19, 1);

            // Eyes: always open for ghost
            func_8033A45C(0x1B, 1);
            func_8033A45C(0x1D, 1);
            func_8033A45C(0x1F, 1);
            func_8033A45C(0x21, 1);
            func_8033A45C(0x1A, 1);
            func_8033A45C(0x1C, 1);
            func_8033A45C(0x1E, 1);
            func_8033A45C(0x20, 1);

            // Body parts: visible
            func_8033A45C(0x22, 1);
            func_8033A45C(0x24, 1);
            func_8033A45C(0x26, 1);
            func_8033A45C(0x28, 1);
            func_8033A45C(0x23, 1);
            func_8033A45C(0x25, 1);
            func_8033A45C(0x27, 1);
            func_8033A45C(0x29, 1);
            break;
        }
    }
}


static void ghost_ensure_init(u32 pid) {
    GhostModel *gm = &ghost_models[pid];
    if (gm->initialized) return;

    gm->shadow_model = assetcache_get(ASSET_3BF_MODEL_PLAYER_SHADOW);
    gm->bone_save = boneTransformList_new();
    if (!gm->bone_save) return;

    gm->bone_blend_temp = boneTransformList_new();
    if (!gm->bone_blend_temp) return;

    gm->bone_fade_from = boneTransformList_new();
    if (!gm->bone_fade_from) return;

    gm->bones_world = func_8034A2C8();   /* world-space bone sink (own buffer) */
    if (!gm->bones_world) return;

    gm->current_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->bone_blend_base_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->smooth_yaw = 0.0f;
    gm->ghost_timer = 0.0f;
    gm->ground_y = 0.0f;
    gm->initialized = TRUE;
}

// === Transformation model helpers ===

// Returns the model asset ID for a given transformation.
// Always returns a valid asset — ghosts must never fall back to baModelBin
// because baModelBin reflects the LOCAL player's transformation, not the ghost's.
static enum asset_e ghost_model_for_transformation(u8 transformation) {
    switch (transformation) {
        case TRANSFORM_2_TERMITE:   return ASSET_34F_MODEL_BANJO_TERMITE;
        case TRANSFORM_3_PUMPKIN:   return ASSET_36F_MODEL_BANJO_PUMPKIN;
        case TRANSFORM_4_WALRUS:    return ASSET_359_MODEL_BANJO_WALRUS;
        case TRANSFORM_5_CROC:      return ASSET_374_MODEL_BANJO_CROC;
        case TRANSFORM_6_BEE:       return ASSET_362_MODEL_BANJO_BEE;
        case TRANSFORM_7_WISHWASHY: return ASSET_356_MODEL_BANJO_WISHYWASHY;
        default:                    return ASSET_34D_MODEL_BANJOKAZOOIE_LOW_POLY;
    }
}

// Update the cached transformation model for a ghost.
// Always loads an explicit model — never relies on baModelBin, which
// reflects the LOCAL player's state and would bleed transformations.
static void ghost_update_xform_model(GhostModel *gm, u8 transformation) {
    if (gm->cached_transformation == transformation && gm->xform_model_bin) return;

    // Release old model if any
    if (gm->xform_model_bin) {
        assetcache_release(gm->xform_model_bin);
        gm->xform_model_bin = (void*)0;
    }

    gm->cached_transformation = transformation;
    enum asset_e asset_id = ghost_model_for_transformation(transformation);
    gm->xform_model_bin = assetcache_get(asset_id);
    gm->cached_model_id = asset_id;
}

// === Ghost transformation particles ===

static void ghost_xform_particles_start(GhostModel *gm) {
    if (gm->xform_particles_active) return;

    gm->xform_emit_blue = partEmitMgr_newEmitter(0x32);
    particleEmitter_setSprite(gm->xform_emit_blue, ASSET_476_SPRITE_BLUE_GLOW);
    particleEmitter_setAccelerationRange(gm->xform_emit_blue, 0.0f, -50.0f, 0.0f, 0.0f, -50.0f, 0.0f);
    particleEmitter_setFade(gm->xform_emit_blue, 0.4f, 0.8f);
    particleEmitter_setFinalScaleRange(gm->xform_emit_blue, 0.03f, 0.03f);
    particleEmitter_setAngularVelocityRange(gm->xform_emit_blue, 0.0f, 0.0f, 300.0f, 0.0f, 0.0f, 300.0f);
    particleEmitter_setParticleLifeTimeRange(gm->xform_emit_blue, 0.65f, 0.65f);
    particleEmitter_setStartingScaleRange(gm->xform_emit_blue, 0.35f, 0.35f);
    func_802EFF50(gm->xform_emit_blue, 1.0f);

    gm->xform_emit_yellow = partEmitMgr_newEmitter(0x32);
    particleEmitter_setSprite(gm->xform_emit_yellow, ASSET_477_SPRITE_YELLOW_GLOW);
    particleEmitter_setAccelerationRange(gm->xform_emit_yellow, 0.0f, -50.0f, 0.0f, 0.0f, -50.0f, 0.0f);
    particleEmitter_setFade(gm->xform_emit_yellow, 0.4f, 0.8f);
    particleEmitter_setFinalScaleRange(gm->xform_emit_yellow, 0.03f, 0.03f);
    particleEmitter_setAngularVelocityRange(gm->xform_emit_yellow, 0.0f, 0.0f, 300.0f, 0.0f, 0.0f, 300.0f);
    particleEmitter_setParticleLifeTimeRange(gm->xform_emit_yellow, 0.65f, 0.65f);
    particleEmitter_setStartingScaleRange(gm->xform_emit_yellow, 0.35f, 0.35f);
    func_802EFF50(gm->xform_emit_yellow, 1.0f);

    gm->xform_orbit_progress = 0.0f;
    gm->xform_particles_active = TRUE;
}

static void ghost_xform_particles_stop(GhostModel *gm) {
    if (!gm->xform_particles_active) return;
    partEmitMgr_freeEmitter(gm->xform_emit_blue);
    partEmitMgr_freeEmitter(gm->xform_emit_yellow);
    gm->xform_emit_blue = (ParticleEmitter*)0;
    gm->xform_emit_yellow = (ParticleEmitter*)0;
    gm->xform_particles_active = FALSE;
}

// Update orbiting particles at ghost position (replicates func_802AF900 from dronexform.c)
static void ghost_xform_particles_update(GhostModel *gm, f32 gx, f32 gy, f32 gz) {
    if (!gm->xform_particles_active) return;

    f32 dt = time_getDelta();
    gm->xform_orbit_progress += dt * 0.36f;  // ~0.36 = matches original orbit speed
    if (gm->xform_orbit_progress > 1.0f) gm->xform_orbit_progress -= 1.0f;

    f32 t = gm->xform_orbit_progress;
    f32 radius = 55.0f;
    f32 angle1 = t * 6.2831853f;
    f32 s1 = sinf(angle1);
    f32 c1 = cosf(angle1);

    // Yellow glow — orbit position 1
    f32 pos1[3];
    pos1[0] = gx + s1 * radius;
    pos1[1] = gy + ml_interpolate_f(t, 0.0f, 130.0f);
    pos1[2] = gz + c1 * radius;
    particleEmitter_setParticleVelocityRange(gm->xform_emit_yellow, s1*30.0f, 10.0f, c1*30.0f, s1*30.0f, 10.0f, c1*30.0f);
    particleEmitter_setPosition(gm->xform_emit_yellow, pos1);
    particleEmitter_emitN(gm->xform_emit_yellow, 1);

    // Blue glow — orbit position 2 (opposite side)
    f32 angle2 = (1.0f - ml_remainder_f(t + 0.5f, 1.0f)) * 6.2831853f;
    f32 pos2[3];
    pos2[0] = gx - sinf(angle2) * radius;
    pos2[1] = gy + ml_interpolate_f(t, 130.0f, 0.0f);
    pos2[2] = gz - cosf(angle2) * radius;
    particleEmitter_setParticleVelocityRange(gm->xform_emit_blue, s1*30.0f, 10.0f, c1*30.0f, s1*30.0f, 10.0f, c1*30.0f);
    particleEmitter_setPosition(gm->xform_emit_blue, pos2);
    particleEmitter_emitN(gm->xform_emit_blue, 1);
}

// Spawn a visual-only egg particle at the ghost's position.
// Uses the game's commonParticle system, then relocates to ghost pos/yaw.
static void ghost_spawn_egg(f32 gx, f32 gy, f32 gz, f32 gyaw, s32 type) {
    // type: 1 = head egg (forward), 4 = ass egg (backward)
    s32 idx = commonParticle_new(type, 1);
    if (idx < 0) return;

    CommonParticle *p = commonParticle_getCurrentParticle();
    u8 proj_idx = p->projectileIndex;
    u8 phys_idx = p->unk47;

    f32 rad = gyaw * (3.14159265f / 180.0f);
    f32 fwd_x = sinf(rad);
    f32 fwd_z = cosf(rad);

    f32 egg_pos[3];
    f32 egg_vel[3];

    if (type == 1) {
        // Head egg: spawn in front of ghost, fly forward
        egg_pos[0] = gx + fwd_x * 70.0f;
        egg_pos[1] = gy + 80.0f;
        egg_pos[2] = gz + fwd_z * 70.0f;
        egg_vel[0] = fwd_x * 800.0f;
        egg_vel[1] = 0.0f;
        egg_vel[2] = fwd_z * 800.0f;
    } else {
        // Ass egg: spawn behind ghost, lob backward+up
        egg_pos[0] = gx - fwd_x * 18.0f;
        egg_pos[1] = gy + 60.0f;
        egg_pos[2] = gz - fwd_z * 18.0f;
        egg_vel[0] = -fwd_x * 200.0f;
        egg_vel[1] = 710.0f;
        egg_vel[2] = -fwd_z * 200.0f;
    }

    projectile_setPosition(proj_idx, egg_pos);
    func_80344D94(phys_idx, egg_pos);
    func_80344E3C(phys_idx, egg_vel);
}

// Spawn a decorative feather actor at ghost position (replicates func_802D8B20)
static void ghost_spawn_feather(f32 gx, f32 gy, f32 gz, f32 gyaw, bool gold) {
    f32 pos[3] = {gx, gy, gz};
    s32 yaw_offset = (randf() > 0.5f) ? 30 : -30;
    // 0x1FF = red feather visual, 0x200 = gold feather visual
    Actor *feather = actor_spawnWithYaw_f32(gold ? 0x200 : 0x1FF, pos, (s32)(gyaw + yaw_offset));
    if (feather) {
        func_8032AA58(feather, 0.45f);
        feather->actor_specific_1_f = 22.0f;
        feather->unk1C[1] = 48.0f;
        feather->lifetime_value = 1.2f;
    }
}

void bkrecomp_net_draw_ghosts(Gfx **gfx, Mtx **mtx, Vtx **vtx) {
    if (!recomp_net_is_connected()) return;
    if (!baModelBin) return;

    u32 local_id = recomp_net_get_local_player_id();
    u32 local_map = (u32)map_get();

    // Push camera state to C++ for nametag projection
    {
        f32 cam_pos[3], cam_rot[3];
        viewport_getPosition_vec3f(cam_pos);
        viewport_getRotation_vec3f(cam_rot);
        struct {
            f32 pos[3];
            f32 rot[3];
            f32 fov_y;
            f32 near_plane;
            s32 fb_width;
            s32 fb_height;
            f32 viewport_aspect;
            u32 map_id;
        } cam_data;
        cam_data.pos[0] = cam_pos[0]; cam_data.pos[1] = cam_pos[1]; cam_data.pos[2] = cam_pos[2];
        cam_data.rot[0] = cam_rot[0]; cam_data.rot[1] = cam_rot[1]; cam_data.rot[2] = cam_rot[2];
        cam_data.fov_y = viewport_getFOVy();
        cam_data.near_plane = viewport_getNear();
        cam_data.fb_width = gFramebufferWidth;
        cam_data.fb_height = gFramebufferHeight;
        cam_data.viewport_aspect = viewport_getAspectRatio();
        cam_data.map_id = local_map;
        recomp_net_push_camera_state(&cam_data);
    }

    // Snapshot ALL local player render state BEFORE any ghost rendering.
    s32 saved_nodes[0x2A];
    s32 i;
    for (i = 0; i < 0x2A; i++) saved_nodes[i] = func_8033A0F0(i);

    /* baModel_draw already ran anim_update; animcache still holds this frame's
     * local bone buffer — do NOT call baanim_80289F30 here (that ran before the
     * shadow draw below would NULL modelRenderBoneTransformList via
     * modelRender_reset, and a second anim_update double-advances local anims).
     * Per-ghost: shadow modelRender_draw clears BoneTransformList global; we
     * must modelRender_setBoneTransformList(bone_buffer) again before the body. */

    AnimMtxList *saved_anim_mtx_list = D_8038371C;

    for (u32 pid = 0; pid < MAX_PLAYERS; pid++) {
        if (pid == local_id) continue;

        RemoteState rs;
        if (!recomp_net_get_remote_state(pid, &rs)) continue;
        if (rs.map_id != local_map || rs.x < -15000.0f) continue;

        /* Carry transition side effects — only when the ghost's map matches
         * ours, otherwise we'd despawn/spawn actors that aren't relevant
         * here. We're already past the `rs.map_id != local_map` guard. */
        {
            u8 prev = ghost_prev_carry_kind[pid];
            if (prev != rs.carry_kind) {
                if (prev == NET_CARRY_KIND_NONE && rs.carry_kind == NET_CARRY_KIND_ORANGE) {
                    /* Carrier just picked up — despawn the world copy of
                     * the orange (it was sitting on the tree on every
                     * peer's machine since FLAG_1 isn't reliably synced). */
                    Actor *orange = actorArray_findActorFromMarkerId(MARKER_36_ORANGE_COLLECTIBLE);
                    if (orange && orange->marker) {
                        marker_despawn(orange->marker);
                    }
                } else if (prev == NET_CARRY_KIND_ORANGE && rs.carry_kind == NET_CARRY_KIND_NONE) {
                    /* Carrier just threw / dropped. Mirror the host's
                     * landing target exactly: lmonkey calls
                     * func_8028FA34(0xc6, chimpy) → func_8028DEEC →
                     * set_throw_target_position(nodeprop), so the orange
                     * lands at Chimpy's "throw target" node prop (actor
                     * id 0xc6 anchored on Chimpy). Use the same lookup
                     * here to spawn at the identical world-space spot.
                     * Fallbacks: if the node lookup fails, use Chimpy's
                     * actor position; if Chimpy isn't loaded (peer in
                     * another sub-area), use the ghost's position. The
                     * vanilla collectible self-despawns on
                     * FLAG_3_CHIMPY_HAS_LEAVED. */
                    if (!actorArray_findActorFromMarkerId(MARKER_36_ORANGE_COLLECTIBLE)) {
                        Actor *chimpy = actorArray_findActorFromMarkerId(MARKER_A_CHIMPY);
                        s32 spawn_pos[3];
                        bool placed = FALSE;
                        if (chimpy) {
                            NodeProp *throw_node =
                                nodeprop_findByActorIdAndActorPosition((enum actor_e)0xc6, chimpy);
                            if (throw_node) {
                                f32 nps[3];
                                nodeprop_getPosition(throw_node, nps);
                                spawn_pos[0] = (s32)nps[0];
                                spawn_pos[1] = (s32)nps[1];
                                spawn_pos[2] = (s32)nps[2];
                                placed = TRUE;
                            } else {
                                spawn_pos[0] = (s32)chimpy->position[0];
                                spawn_pos[1] = (s32)chimpy->position[1];
                                spawn_pos[2] = (s32)chimpy->position[2];
                                placed = TRUE;
                            }
                        }
                        if (!placed) {
                            spawn_pos[0] = (s32)rs.x;
                            spawn_pos[1] = (s32)rs.y;
                            spawn_pos[2] = (s32)rs.z;
                        }
                        actor_spawnWithYaw_s32(ACTOR_29_ORANGE_COLLECTIBLE,
                            spawn_pos, (s32)rs.yaw);
                    }
                }
                ghost_prev_carry_kind[pid] = rs.carry_kind;
            }
        }

        ghost_ensure_init(pid);
        GhostModel *gm = &ghost_models[pid];
        if (!gm->initialized) continue;

        bool ghost_leaving_skid = (gm->prev_bs_state == BS_SKID && rs.bs_state != BS_SKID);

        /* States whose EXIT transition needs a bone crossfade. Two reasons
         * a bs_state lands here:
         *  (a) Multi-phase chained clips inside one bs_state (BFLIP enter→
         *      hold→exit, BPECK peck→fall) where adjacent clips have very
         *      different poses.
         *  (b) States whose exit clip cross-fades into IDLE on the local
         *      via Animation.duration; the wire-side snap-to-clip-b
         *      otherwise compresses the visible exit (Z-crouch stand-up,
         *      LAND→IDLE residual after FALL/BFLIP).
         * BFLIP / BPECK / FALL: case (a). LANDING, CROUCH, BFLAP: case (b).
         * BFLAP is the Kazooie-out double jump (A+A); the local cross-fades
         * Kazooie back into the backpack on landing — without crossfade the
         * ghost snaps her away in one frame. */
        bool in_complex_move = (gm->prev_bs_state == BS_12_BFLIP
            || gm->prev_bs_state == BS_11_BPECK
            || gm->prev_bs_state == BS_2F_FALL
            || gm->prev_bs_state == BS_20_LANDING
            || gm->prev_bs_state == BS_CROUCH
            || gm->prev_bs_state == BS_BFLAP);

        // === Update transformation model cache ===
        ghost_update_xform_model(gm, rs.transformation);

        // === Direct animation mirror from local player's AnimCtrl ===
        // Direct mirror: animation and timer come straight from sender's AnimCtrl.
        // No ghost-side blending needed — the sender's AnimCtrl already handles
        // smooth transitions (transition_duration) and the timer reflects that.
        gm->current_anim = rs.animation_id;
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
            // Bbuster impact: fire once when ghost first touches ground during bbuster.
            // Game triggers func_8029FB30 in bsbbuster_update case 2 on player_isStable().
            // Reset flag when entering bbuster, fire once on ground contact.
            if (rs.bs_state == BS_F_BBUSTER && gm->prev_bs_state != BS_F_BBUSTER) {
                gm->bbuster_dust_done = FALSE;
            }
            f32 prev_height = gm->prev_y - gm->ground_y;
            bool was_airborne = (prev_height >= 30.0f);
            bool bbuster_land = (rs.bs_state == BS_F_BBUSTER && on_ground
                && was_airborne && !gm->bbuster_dust_done);
            bool is_sliding = (rs.bs_state == BS_SLIDE);
            bool is_barge = (rs.bs_state == BS_BBARGE);
            bool btrot_start = (rs.bs_state == BS_16_BTROT_WALK &&
                (gm->prev_bs_state == BS_15_BTROT_IDLE || gm->prev_bs_state == BS_14_BTROT_ENTER));
            bool btrot_walking = (rs.bs_state == BS_16_BTROT_WALK);

            f32 dust_pos[3] = {rs.x, rs.y + 10.0f, rs.z};

            if (on_ground && gm->dust_cooldown == 0) {
                if (bbuster_land) {
                    // Mirrors func_8029FB30: 2 rings of 6 puffs each at 60° intervals
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
                    gm->bbuster_dust_done = TRUE;
                    gm->dust_cooldown = 15;
                } else if (is_sliding) {
                    f32 slide_pos[3] = {rs.x, rs.y + 20.0f, rs.z};
                    gm->slide_dust_phase++;
                    if (gm->slide_dust_phase >= 3) gm->slide_dust_phase = 0;
                    if (gm->slide_dust_phase != 0) {
                        f32 side_offset[3];
                        f32 side_angle = mlNormalizeAngle(rs.yaw + 90.0f);
                        func_802589E4(side_offset, side_angle, randf() * 10.0f + 20.0f);
                        side_offset[1] = 0.0f;
                        if (gm->slide_dust_phase == 1) {
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

        // === Transformation particles for ghost ===
        // Use Mumbo lock state: if this ghost's player owns the lock, they're transforming
        {
            bool is_transforming = bkrecomp_net_mumbo_is_locked()
                                   && bkrecomp_net_mumbo_get_lock_owner() == (u8)pid;

            if (is_transforming && !gm->xform_particles_active) {
                ghost_xform_particles_start(gm);
            } else if (!is_transforming && gm->xform_particles_active) {
                ghost_xform_particles_stop(gm);
            }

            if (gm->xform_particles_active) {
                ghost_xform_particles_update(gm, rs.x, rs.y, rs.z);
            }
        }

        // === Egg projectile visual for ghost ===
        {
            bool is_egg_head = (rs.bs_state == BS_9_EGG_HEAD);
            bool is_egg_ass  = (rs.bs_state == BS_A_EGG_ASS);
            bool was_egg     = (gm->prev_bs_state == BS_9_EGG_HEAD || gm->prev_bs_state == BS_A_EGG_ASS);

            // Reset fire flag when entering egg state
            if ((is_egg_head || is_egg_ass) && !was_egg) {
                gm->egg_fired = FALSE;
            }

            // Spawn egg at the trigger animation frame
            if (!gm->egg_fired) {
                if (is_egg_head && rs.anim_timer >= 0.47f) {
                    ghost_spawn_egg(rs.x, rs.y, rs.z, rs.yaw, 1);
                    gm->egg_fired = TRUE;
                } else if (is_egg_ass && rs.anim_timer >= 0.38f) {
                    ghost_spawn_egg(rs.x, rs.y, rs.z, rs.yaw, 4);
                    gm->egg_fired = TRUE;
                }
            }
        }

        // === Feather particles for ghost (flying = red, wonderwing = gold) ===
        {
            bool is_flying = (rs.bs_state == BS_24_FLY);
            bool is_wonderwing = (rs.bs_state == BS_1A_WONDERWING_ENTER
                               || rs.bs_state == BS_1B_WONDERWING_IDLE
                               || rs.bs_state == BS_1C_WONDERWING_WALK
                               || rs.bs_state == BS_1D_WONDERWING_JUMP);

            if (gm->feather_cooldown > 0) gm->feather_cooldown--;

            if (gm->feather_cooldown == 0) {
                if (is_flying) {
                    ghost_spawn_feather(rs.x, rs.y, rs.z, rs.yaw, FALSE);
                    gm->feather_cooldown = 15;  // ~every 0.5s at 30fps
                } else if (is_wonderwing) {
                    ghost_spawn_feather(rs.x, rs.y, rs.z, rs.yaw, TRUE);
                    gm->feather_cooldown = 10;  // slightly faster for gold
                }
            }
        }

        gm->prev_bs_state = rs.bs_state;
        gm->prev_y = rs.y;

        // Yaw compensation: btrot and longleg use PLAYER_MODEL_DIR_KAZOOIE which
        // flips the model 180°. The sender's yaw_get() includes this flip (+180°),
        // but the ghost renders without baModelDirection. Undo the flip for these states.
        f32 target_yaw = rs.yaw;
        bool kazooie_direction = (rs.bs_state == BS_14_BTROT_ENTER
            || rs.bs_state == BS_15_BTROT_IDLE || rs.bs_state == BS_16_BTROT_WALK
            || rs.bs_state == BS_17_BTROT_EXIT || rs.bs_state == BS_8_BTROT_JUMP
            || rs.bs_state == BS_25_LONGLEG_ENTER || rs.bs_state == BS_26_LONGLEG_IDLE
            || rs.bs_state == BS_LONGLEG_WALK || rs.bs_state == BS_LONGLEG_JUMP
            || rs.bs_state == BS_LONGLEG_EXIT);
        if (kazooie_direction) {
            target_yaw = mlNormalizeAngle(target_yaw - 180.0f);
        }

        /* Skid / bsturn_end: sender snaps yaw (often ~180°). smooth_yaw lag vs
         * network yaw twists the mesh against animation roots → stretched limbs.
         * Match old ghost path (animcache stomp era): stay locked to net yaw
         * during skid and the first walk frame after leaving it. */
        f32 yaw_diff = target_yaw - gm->smooth_yaw;
        while (yaw_diff > 180.0f) yaw_diff -= 360.0f;
        while (yaw_diff < -180.0f) yaw_diff += 360.0f;
        bool large_yaw_change = (yaw_diff > 90.0f || yaw_diff < -90.0f);

        if (rs.bs_state == BS_SKID || ghost_leaving_skid) {
            gm->smooth_yaw = target_yaw;
        } else if (large_yaw_change) {
            gm->smooth_yaw = lerp_angle(gm->smooth_yaw, target_yaw, 1.0f);
        } else {
            gm->smooth_yaw = lerp_angle(gm->smooth_yaw, target_yaw, 0.25f);
        }

        // Ground tracking
        bool on_ground = (rs.bs_state == BS_1_IDLE || rs.bs_state == BS_0_NONE
            || rs.bs_state == BS_WALK || rs.bs_state == BS_2_WALK_SLOW
            || rs.bs_state == BS_4_WALK_FAST || rs.bs_state == BS_WALK_CREEP
            || rs.bs_state == BS_CROUCH || rs.bs_state == BS_CLAW
            || rs.bs_state == BS_SKID || rs.bs_state == BS_ROLL
            || rs.bs_state == BS_15_BTROT_IDLE || rs.bs_state == BS_16_BTROT_WALK
            || rs.bs_state == BS_20_LANDING);
        if (on_ground) gm->ground_y = rs.y;

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

        {
            /* Same effective sampling as the old animcache-stomp ghost path (raw
             * animationFile_getBoneTransformList into the active bone buffer).
             * We cannot call baanim_80289F30 per ghost — anctrl_drawSetup runs
             * anim_update and would advance the LOCAL player's clips N times.
             * Network-sourced Animation.duration blending regressed skid visuals;
             * keep bones purely from clip + timer until we have a matrix-only setup path. */
            void *anim_file = animBinCache_get(gm->current_anim);
            if (anim_file) {
                bool anim_changed = (gm->bone_blend_base_anim != gm->current_anim);
                if (anim_changed) {
                    /* On phase change inside BFLIP / BPECK / FALL, capture the
                     * just-rendered bones as the fade-from pose. The crossfade
                     * below blends into the new clip over fade_duration so
                     * sub-50ms phase swaps don't pop. Outside these states we
                     * keep the old hard-cut behaviour to limit blast radius. */
                    if (in_complex_move && gm->bone_fade_from) {
                        boneTransformList_interpolate(gm->bone_fade_from,
                            gm->bone_save, gm->bone_save, 0.0f);
                        gm->fade_progress = 0.0f;
                        gm->fade_active = TRUE;
                    }
                    /* Reset always now — the crossfade carries continuity for
                     * complex moves; non-complex moves keep their original
                     * reset-then-sample path. */
                    boneTransformList_reset(gm->bone_blend_temp);
                    gm->bone_blend_base_anim = gm->current_anim;
                }
                animationFile_getBoneTransformList(anim_file, gm->ghost_timer, gm->bone_blend_temp);

                if (gm->fade_active && gm->bone_fade_from) {
                    /* ~150ms fade duration: covers ~3 snapshot intervals at
                     * 20Hz send. Long enough to mask a missed intermediate
                     * phase clip AND smear over short LAND clips whose
                     * 20Hz-sampled timer otherwise looks like a mini-loop
                     * before IDLE. Without this we'd need to interpolate
                     * anim_timer on the C++ side (broader change). */
                    f32 fade_duration = 0.15f;
                    gm->fade_progress += time_getDelta() / fade_duration;
                    if (gm->fade_progress >= 1.0f) {
                        gm->fade_progress = 1.0f;
                        gm->fade_active = FALSE;
                    }
                    boneTransformList_interpolate(gm->bone_save,
                        gm->bone_fade_from, gm->bone_blend_temp,
                        gm->fade_progress);
                } else {
                    boneTransformList_interpolate(gm->bone_save, gm->bone_blend_temp,
                        gm->bone_blend_temp, 0.0f);
                }
            }

#if BKRECOMP_NET_GHOST_ANIM_LOG
            /* Frame-by-frame dump while the ghost is in SKID, plus the first
             * frame after leaving SKID (ghost_leaving_skid). All wire fields
             * applied to the ghost this frame so we can compare against the
             * sender log line-by-line. */
            if (rs.bs_state == BS_SKID || ghost_leaving_skid) {
                recomp_printf(
                    "[GhostAnim/RECV] pid=%u bs=%u leave=%d anim=%u t=%.5f dur=%.5f pb=%u sub=[%.4f,%.4f] hvel=%.2f\n",
                    (unsigned)pid, (unsigned)rs.bs_state, (int)ghost_leaving_skid,
                    (unsigned)rs.animation_id, rs.anim_timer, rs.anim_duration,
                    (unsigned)rs.anim_playback_type,
                    rs.anim_subrange_start, rs.anim_subrange_end,
                    rs.horizontal_velocity);
            }
#endif

            /* Shadow draw above called modelRender_reset → BoneTransformList NULL. */
            modelRender_setBoneTransformList(gm->bone_save);
            func_8033A444((void*)0);
        }

        {
            s32 env_color[3];
            func_8029A47C(env_color);
            modelRender_setEnvColor(env_color[0], env_color[1], env_color[2], 255);
        }
        func_8033A280(2.0f);
        /* Route world-space bone writes for this draw into the ghost's
         * own struct5Bs (not D_80363780, which belongs to the local
         * player and would be clobbered — that's why this call was
         * originally omitted). With a per-ghost buffer, the renderer
         * can populate ghost bone positions for our use without
         * corrupting collision or other systems that read the local
         * player's bones. */
        func_8033A450(gm->bones_world);
        modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
        ghost_setup_all_model_nodes(rs.transformation, kazooie_head, kazooie_wings, kazooie_feet);
        bkrecomp_setup_custom_skinning(&ghost_skinning_data[pid], (u32)gm->cached_model_id);
        modelRender_draw(gfx, mtx, pos, rot, rs.scale, ref, gm->xform_model_bin);

        /* Carried-prop visual replication. Mirrors bacarry_update on the
         * sender: the deliverer's local orange follows
         * baModel_getPosition, which averages bones 5 and 6 (~body
         * center). We just rendered the ghost with bone output going
         * to gm->bones_world, so reading bones 5/6 from there gives
         * the same world-space body-center position — including the
         * idle-bob and walk-bob already baked into the animation. */
        if (rs.carry_kind == NET_CARRY_KIND_ORANGE) {
            if (!s_carry_orange_model_bin) {
                s_carry_orange_model_bin = assetcache_get(ASSET_2D2_MODEL_ORANGE);
            }
            if (s_carry_orange_model_bin) {
                f32 b5[3], b6[3];
                func_8034A174(gm->bones_world, 5, b5);
                func_8034A174(gm->bones_world, 6, b6);
                f32 op[3] = {
                    (b5[0] + b6[0]) * 0.5f,
                    (b5[1] + b6[1]) * 0.5f,
                    (b5[2] + b6[2]) * 0.5f
                };
                /* Fallback if the bones are still zeroed (first frame
                 * before the renderer wrote them, or unexpected animation
                 * with no skin output): anchor above the ghost's feet. */
                if (op[0] == 0.0f && op[1] == 0.0f && op[2] == 0.0f) {
                    op[0] = rs.x;
                    op[1] = rs.y + 100.0f;
                    op[2] = rs.z;
                }
                f32 orot[3] = {0.0f, gm->smooth_yaw, 0.0f};
                f32 oref[3] = {0.0f, 0.0f, 0.0f};
                cur_drawn_model_transform_id =
                    GHOST_TRANSFORM_ID_START + (pid * GHOST_TRANSFORM_ID_STRIDE) + 1;
                modelRender_setAlpha(0xFF);
                modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
                modelRender_draw(gfx, mtx, op, orot, 1.1f, oref, s_carry_orange_model_bin);
            }
        }

    }

    func_8033A450(D_80363780);
    func_8033A444(saved_anim_mtx_list);

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
