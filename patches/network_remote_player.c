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

// Low-level: bypass AnimCtrl, call animation pipeline directly
extern void *animBinCache_get(enum asset_e asset_id);
extern void animationFile_getBoneTransformList(void *anim_file, f32 progress, void *bone_list);
extern void *boneTransformList_new(void);
extern void modelRender_setBoneTransformList(void *bone_list);

#define MAX_PLAYERS 4

typedef struct {
    void *shadow_model;
    void *bone_list;     // Raw BoneTransformList, no AnimCtrl
    u16 current_anim;
    f32 smooth_yaw;
    f32 ghost_timer;
    f32 anim_duration;
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
    gm->bone_list = boneTransformList_new();
    if (!gm->bone_list) return;

    // Fill with idle animation at frame 0
    void *anim_file = animBinCache_get(ASSET_6F_ANIM_BSSTAND_IDLE);
    if (anim_file) {
        animationFile_getBoneTransformList(anim_file, 0.0f, gm->bone_list);
    }

    gm->current_anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    gm->smooth_yaw = 0.0f;
    gm->ghost_timer = 0.0f;
    gm->anim_duration = 6.0f;
    gm->initialized = TRUE;
    recomp_printf("[NetGhost] Ghost %d init (raw bones)\n", pid);
}

// BS state -> animation mapping
static void ghost_sync_anim(GhostModel *gm, u8 bs_state) {
    if (!gm->initialized) return;

    u16 anim = ASSET_6F_ANIM_BSSTAND_IDLE;
    f32 duration = 6.0f;

    switch (bs_state) {
        case BS_0_NONE: case BS_1_IDLE: case BS_20_LANDING:
            anim = ASSET_6F_ANIM_BSSTAND_IDLE; duration = 6.0f; break;
        case BS_D_TIMEOUT: case BS_53_TIMEOUT:
            anim = ASSET_77_ANIM_BSTIMEOUT; duration = 4.0f; break;
        case BS_2_WALK_SLOW: case BS_WALK_CREEP:
            anim = ASSET_2_ANIM_BSWALK_CREEP; duration = 2.0f; break;
        case BS_WALK:  anim = ASSET_3_ANIM_BSWALK; duration = 1.2f; break;
        case BS_4_WALK_FAST: anim = ASSET_C_ANIM_BSWALK_RUN; duration = 0.8f; break;
        case BS_SKID:  anim = ASSET_E_ANIM_BSTURN; duration = 0.4f; break;
        case BS_SLIDE: anim = ASSET_5A_ANIM_BSSLIDE_FRONT; duration = 1.5f; break;
        case BS_ROLL:  anim = ASSET_11_ANIM_BSWHIRL_WALK; duration = 0.5f; break;
        case BS_5_JUMP: anim = ASSET_8_ANIM_BSJUMP; duration = 2.0f; break;
        case BS_12_BFLIP: anim = ASSET_4C_ANIM_BSBFLIP_HOLD; duration = 1.5f; break;
        case BS_2F_FALL: anim = ASSET_B0_ANIM_BSJUMP_FALL; duration = 2.0f; break;
        case BS_3D_FALL_TUMBLING: anim = ASSET_68_ANIM_BSJUMP_TUMBLE; duration = 0.8f; break;
        case BS_CLAW:  anim = ASSET_5_ANIM_BSPUNCH; duration = 1.2f; break;
        case BS_F_BBUSTER: anim = ASSET_1D_ANIM_BSBBUSTER; duration = 1.2f; break;
        case BS_BFLAP: anim = ASSET_17_ANIM_BSBFLAP; duration = 1.2f; break;
        case BS_11_BPECK: anim = ASSET_1A_ANIM_BSBPECK; duration = 1.0f; break;
        case BS_BBARGE: anim = ASSET_1C_ANIM_BSBBARGE; duration = 1.2f; break;
        case BS_CROUCH: anim = ASSET_10C_ANIM_BSCROUCH_IDLE; duration = 6.0f; break;
        case BS_9_EGG_HEAD: anim = ASSET_2A_ANIM_BSEGGHEAD; duration = 1.0f; break;
        case BS_A_EGG_ASS: anim = ASSET_2B_ANIM_BSEGGASS; duration = 1.0f; break;
        case BS_15_BTROT_IDLE: anim = ASSET_26_ANIM_BSBTROT_IDLE; duration = 6.0f; break;
        case BS_16_BTROT_WALK: anim = ASSET_15_ANIM_BSBTROT_WALK; duration = 1.0f; break;
        case BS_8_BTROT_JUMP: anim = ASSET_27_ANIM_BSBTROR_JUMP; duration = 1.5f; break;
        case BS_24_FLY: anim = ASSET_38_ANIM_BSBFLY; duration = 1.5f; break;
        case BS_2D_SWIM_IDLE: anim = ASSET_57_ANIM_BSSWIM_IDLE; duration = 4.0f; break;
        case BS_2E_SWIM: anim = ASSET_39_ANIM_BSSWIM_MOVE; duration = 1.0f; break;
        case BS_4F_CLIMB_IDLE: anim = ASSET_B1_ANIM_BSCLIMB_IDLE_1; duration = 6.0f; break;
        case BS_50_CLIMB_MOVE: anim = ASSET_A_ANIM_BSCLIMB_MOVE; duration = 1.0f; break;
        case BS_E_OW: anim = ASSET_4D_ANIM_BSOW; duration = 1.0f; break;
        case BS_41_DIE: anim = ASSET_9_ANIM_BSDIE; duration = 3.0f; break;
        case BS_3C_TALK: anim = ASSET_14A_ANIM_BSREST_LISTEN; duration = 4.0f; break;
        default: anim = ASSET_6F_ANIM_BSSTAND_IDLE; duration = 6.0f; break;
    }

    if (anim != gm->current_anim) {
        gm->current_anim = anim;
        gm->anim_duration = duration;
        gm->ghost_timer = 0.0f;
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
        if (!gm->initialized || !gm->bone_list) continue;

        ghost_sync_anim(gm, rs.bs_state);

        // Load animation frame directly into our raw bone list
        void *anim_file = animBinCache_get(gm->current_anim);
        if (anim_file) {
            animationFile_getBoneTransformList(anim_file, gm->ghost_timer, gm->bone_list);
        }

        // Advance timer
        f32 speed = 1.0f / (gm->anim_duration * 60.0f);
        gm->ghost_timer += speed;
        if (gm->ghost_timer >= 1.0f) gm->ghost_timer -= 1.0f;

        // Position / rotation
        gm->smooth_yaw = lerp_angle(gm->smooth_yaw, rs.yaw, 0.25f);
        f32 pos[3] = {rs.x, rs.y, rs.z};
        f32 rot[3] = {rs.pitch, gm->smooth_yaw, 0.0f};
        f32 ref[3] = {0.0f, 0.0f, 0.0f};

        cur_drawn_model_transform_id = GHOST_TRANSFORM_ID_START + (pid * GHOST_TRANSFORM_ID_STRIDE);

        // Shadow FIRST (before setting bone transforms, as modelRender_draw resets them)
        if (gm->shadow_model) {
            f32 sp[3] = {rs.x, rs.y + 4.0f, rs.z};
            f32 sr[3] = {0.0f, 0.0f, 0.0f};
            modelRender_setAlpha(0xFF);
            modelRender_setDepthMode(MODEL_RENDER_DEPTH_COMPARE);
            modelRender_draw(gfx, mtx, sp, sr, 0.43f, 0, gm->shadow_model);
        }

        // Set ghost bone transforms RIGHT BEFORE Banjo render
        // (after shadow, which may reset modelRenderBoneTransformList)
        modelRender_setBoneTransformList(gm->bone_list);

        // Banjo
        s32 env_color[3];
        func_8029A47C(env_color);
        modelRender_setEnvColor(env_color[0], env_color[1], env_color[2], 255);
        func_8033A280(2.0f);
        func_8033A450(D_80363780);
        modelRender_setDepthMode(MODEL_RENDER_DEPTH_FULL);
        modelRender_draw(gfx, mtx, pos, rot, baModelScale, ref, baModelBin);
    }

    // Restore local player bone transforms
    baanim_80289F30();
}

RECOMP_EXPORT void bkrecomp_net_manage_ghosts(void) {}
RECOMP_EXPORT void bkrecomp_net_ghost_init(void) {
    recomp_printf("[NetGhost] Ghost system initialized (direct anim)\n");
}
