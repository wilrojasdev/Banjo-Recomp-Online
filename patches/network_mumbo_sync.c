#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
extern u32 recomp_net_is_connected(void);
extern bool bkrecomp_net_mumbo_is_locked(void);
extern bool bkrecomp_net_mumbo_is_local_owner(void);
extern bool bkrecomp_net_mumbo_send_lock(void);
extern void bkrecomp_net_mumbo_send_unlock(void);
extern u8 bkrecomp_net_mumbo_get_lock_owner(void);
extern void recomp_net_send_flag_change(u32 flag_type, u32 flag_index, u32 value, u32 map_id);

#define NET_FLAG_MUMBO_ACTION        7
#define MUMBO_ACTION_DEDUCT_TOKENS   2

// Decomp externs (use types from functions.h where available)
extern enum map_e map_get(void);
extern s32 player_movementGroup(void);
extern void player_getPosition(f32 pos[3]);
extern s32 func_8028F20C(void);
extern s32 func_8028EFC8(void);
extern void func_8028F94C(s32 mode, f32 *pos);
extern void func_8028F918(s32 mode);
extern bool func_8028FB88(enum transformation_e xform_id);
extern void gcpausemenu_80314AC8(s32 enable);
extern void func_8025A58C(s32 dir, s32 duration);
extern void func_80256E24(f32 *out, f32 a, f32 yaw, f32 c, f32 d, f32 e);
extern void func_8028E668(f32 *pos, f32 a, f32 b, f32 c);

// Functions already declared in functions.h but used here
extern void func_803255FC(Actor *);
extern void func_80325760(Actor *);
extern void subaddie_set_state_looped(Actor *, s32);

// Functions not in headers (from decomp .c files or patched)
extern s32 item_getCount(s32 item);
extern void item_adjustByDiffWithHud(s32 item, s32 diff);
extern s32 fileProgressFlag_get(enum file_progress_e index);
extern void fileProgressFlag_set(enum file_progress_e index, s32 val);
extern s32 fileProgressFlag_getN(enum file_progress_e index, s32 n);
extern s32 fileProgressFlag_getAndSet(enum file_progress_e index, s32 val);
extern s32 levelSpecificFlags_get(s32 index);
extern void func_8025A7DC(enum comusic_e track);

// From code_4A6F0.c — internal functions
extern void chmumbo_func_802D1724(void);
extern void chMumbo_func_802D186C(Actor *this);
extern void chMumbo_func_802D1970(Actor *this);
extern void chMumbo_func_802D1B8C(Actor *this, enum transformation_e transform_id);
extern bool chMumbo_withinHorzDistToPlayer(s32 x, s32 z, s32 dist);
extern bool chMumbo_func_802D181C(s32 arg0);

// Global vars from code_4A6F0.c
extern u8 D_8037DDF0;  // Current transformation ID
extern u8 D_8037DDF1;
extern u8 sHasWarnedBanjoAboutDetransform;
extern u8 D_8037DDF3;

// Yaw toward arbitrary position
extern s32 func_803297C8(Actor *actor, f32 target_pos[3]);

// Network: get remote player position
extern u32 recomp_net_get_local_player_id(void);
typedef struct {
    f32 x, y, z;
    f32 yaw;
    u8 _rest[0x30];
} NetFullState_Pos;
extern u32 recomp_net_get_remote_state(u32 player_id, NetFullState_Pos *out);

// Text callback (from code_4A6F0.c)
extern void __chMumbo_textCallback(ActorMarker *caller, enum asset_e text_id, s32 arg2);

// === PATCHED func_8028FB88: Block transformation when Mumbo lock is held by another player ===
// This catches ALL code paths that try to transform the local player
// (pad activation, flag sync side effects, etc.)
extern s32 wishyWashyFlag_get(void);
extern void func_80294AF4(enum transformation_e xform_id);
// bs_checkInterrupt already declared in functions.h with enum type

RECOMP_PATCH bool func_8028FB88(enum transformation_e xform_id) {
    // NET: If another player owns the Mumbo lock, block transformation
    if (recomp_net_is_connected() && bkrecomp_net_mumbo_is_locked()
        && !bkrecomp_net_mumbo_is_local_owner()) {
        return FALSE;
    }
    // Original logic
    if (wishyWashyFlag_get() && xform_id == TRANSFORM_1_BANJO) {
        xform_id = TRANSFORM_7_WISHWASHY;
    }
    func_80294AF4(xform_id);
    return bs_checkInterrupt(BS_INTR_A) == 2;
}

// Transformation cost helpers (replicated from code_4A6F0.c — static, can't extern)
static s32 __transformation_getCost_net(enum transformation_e trans_id) {
    switch (trans_id) {
        case TRANSFORM_2_TERMITE: return 5;
        case TRANSFORM_5_CROC:    return 10;
        case TRANSFORM_4_WALRUS:  return 15;
        case TRANSFORM_3_PUMPKIN: return 20;
        case TRANSFORM_6_BEE:     return 25;
    }
    return 0;
}

static enum file_progress_e __bkProgId_from_transformationId_net(enum transformation_e trans_id) {
    return (trans_id - TRANSFORM_2_TERMITE) + FILEPROG_90_PAID_TERMITE_COST;
}

// === State tracking (per-actor) ===

static Actor *local_mumbo_actor = (Actor*)0;
static s32    local_mumbo_prev_state = 4;
static Actor *remote_anim_mumbo = (Actor*)0;

// Find Mumbo closest to a position
static Actor *find_closest_mumbo(f32 pos[3]) {
    f32 dist;
    return actorArray_findClosestActorFromActorId(pos, ACTOR_7_MUMBO, -1, &dist);
}

// Face Mumbo toward the lock owner
static void face_toward_lock_owner(Actor *this) {
    u8 owner_id = bkrecomp_net_mumbo_get_lock_owner();
    NetFullState_Pos rs;
    if (recomp_net_get_remote_state((u32)owner_id, &rs)) {
        f32 owner_pos[3];
        owner_pos[0] = rs.x;
        owner_pos[1] = rs.y;
        owner_pos[2] = rs.z;
        this->yaw_ideal = (f32)func_803297C8(this, owner_pos);
    }
    func_80328FB0(this, 4.0f);
}

// === Remote animation triggers (called from network_flag_sync.c) ===

RECOMP_EXPORT void bkrecomp_net_mumbo_remote_transform(void) {
    u8 owner_id = bkrecomp_net_mumbo_get_lock_owner();
    NetFullState_Pos rs;
    if (!recomp_net_get_remote_state((u32)owner_id, &rs)) return;

    f32 owner_pos[3];
    owner_pos[0] = rs.x;
    owner_pos[1] = rs.y;
    owner_pos[2] = rs.z;

    Actor *mumbo = find_closest_mumbo(owner_pos);
    if (!mumbo || !mumbo->marker) return;

    remote_anim_mumbo = mumbo;

    if (mumbo->state == 1) {
        // Mumbo is sleeping — play wakeup first, transform will follow in update
        mumbo->marker->propPtr->unk8_3 = TRUE;
        subaddie_set_state_forward(mumbo, 2);
        actor_playAnimationOnce(mumbo);
    } else {
        // Mumbo is already awake — go directly to transform animation
        subaddie_set_state_with_direction(mumbo, 5, 0.0f, 1);
        actor_playAnimationOnce(mumbo);
    }
}

RECOMP_EXPORT void bkrecomp_net_mumbo_remote_idle(void) {
    if (!remote_anim_mumbo) return;

    Actor *mumbo = remote_anim_mumbo;
    if (!mumbo->marker) {
        remote_anim_mumbo = (Actor*)0;
        return;
    }

    // Return to idle (state 4)
    subaddie_set_state(mumbo, 4);
    actor_loopAnimation(mumbo);
    remote_anim_mumbo = (Actor*)0;
}

// === PATCHED UPDATE ===

RECOMP_PATCH void chMumbo_update(Actor *this) {
    s32 face_buttons[6];
    f32 sp4C[3];
    bool sp48;
    bool sp44;
    s32 sp40;
    f32 temp_f12;

    this->unk130 = func_803255FC;
    if (!volatileFlag_get(VOLATILE_FLAG_1)
        && !volatileFlag_get(VOLATILE_FLAG_1F_IN_CHARACTER_PARADE)
        && map_get() != MAP_7A_GL_CRYPT) {
        item_adjustByDiffWithHud(ITEM_1C_MUMBO_TOKEN, 0);
    }

    if (!this->initialized) {
        this->initialized = TRUE;
        this->marker->propPtr->unk8_3 = FALSE;
        this->lifetime_value = 0.0f;
        if (chMumbo_func_802D181C(0x201)) {
            this->lifetime_value = 1.0f;
            subaddie_set_state(this, 7U);
        } else if (chMumbo_func_802D181C(0x202)) {
            this->lifetime_value = 2.0f;
            subaddie_set_state(this, 8U);
        }
    }

    if (!this->volatile_initialized) {
        this->unk38_31 = 0;
        if (player_getTransformation() == TRANSFORM_1_BANJO
            && !fileProgressFlag_get(__bkProgId_from_transformationId_net(D_8037DDF0))
            && (map_get() != MAP_7A_GL_CRYPT)
        ) {
            this->unk38_31 = __transformation_getCost_net(D_8037DDF0);
        }
        this->unk38_0 = (item_getCount(ITEM_1C_MUMBO_TOKEN) >= this->unk38_31);
        this->unk10_12 = 0;
        this->has_met_before = FALSE;
        this->volatile_initialized = TRUE;

        // Reset stale network state for THIS actor
        if (local_mumbo_actor == this) {
            local_mumbo_actor = (Actor*)0;
            local_mumbo_prev_state = 4;
        }
        if (remote_anim_mumbo == this) {
            remote_anim_mumbo = (Actor*)0;
        }
    }

    func_80256E24(sp4C, 0.0f, this->yaw, 0.0f, 0.0f, 10.0f);
    sp4C[0] += this->position[0];
    sp4C[1] += this->position[1];
    sp4C[2] += this->position[2];
    func_8028E668(sp4C, 220.0f, -20.0f, 110.0f);

    // ================================================================
    // NET: TOP-LEVEL LOCK GUARD
    // If another player owns the Mumbo lock, skip all interaction logic.
    // Only allow remote transform animation on the specific Mumbo actor.
    // ================================================================
    if (recomp_net_is_connected()) {
        bool lock_is_remote = bkrecomp_net_mumbo_is_locked()
                              && !bkrecomp_net_mumbo_is_local_owner();

        if (lock_is_remote && !local_mumbo_actor) {
            if (remote_anim_mumbo == this) {
                switch (this->state) {
                    case 2: // Wakeup animation — play then transition to transform
                        this->marker->propPtr->unk8_3 = TRUE;
                        actor_playAnimationOnce(this);
                        face_toward_lock_owner(this);
                        // Wakeup SFX
                        if (actor_animationIsAt(this, 0.25f)) {
                            sfxsource_playHighPriority(0x41);
                        }
                        if (actor_animationIsAt(this, 0.999f)) {
                            // Wakeup done → go to transform animation
                            subaddie_set_state_with_direction(this, 5, 0.0f, 1);
                            actor_playAnimationOnce(this);
                        }
                        break;

                    case 5: // Transform animation — visual only
                        actor_playAnimationOnce(this);
                        face_toward_lock_owner(this);
                        // Sound cues — guard unk44_31 (may not be initialized)
                        if (this->unk44_31 && actor_animationIsAt(this, 0.35f)) {
                            sfxSource_func_8030E2C4(this->unk44_31);
                        }
                        if (actor_animationIsAt(this, 0.01f)) {
                            comusic_playTrack(COMUSIC_1D_MUMBO_TRANSFORMATION);
                        }
                        if (actor_animationIsAt(this, 0.79f)) {
                            func_8025A7DC(COMUSIC_1D_MUMBO_TRANSFORMATION);
                        }
                        if (actor_animationIsAt(this, 0.999f)) {
                            subaddie_set_state(this, 4);
                            actor_loopAnimation(this);
                            remote_anim_mumbo = (Actor*)0;
                        }
                        break;

                    case 4: // Already idle — done
                        remote_anim_mumbo = (Actor*)0;
                        break;
                }
            }
            // All Mumbo actors skip normal logic while remote lock is active
            return;
        }
    }

    // ================================================================
    // NET: LOCAL UNLOCK DETECTION (per-actor)
    // When our transformation sequence finishes (return to state 4),
    // send unlock so the other player can interact.
    // ================================================================
    if (local_mumbo_actor == this) {
        if (this->state == 4 && local_mumbo_prev_state != 4) {
            bkrecomp_net_mumbo_send_unlock();
            local_mumbo_actor = (Actor*)0;
            local_mumbo_prev_state = 4;
        } else {
            local_mumbo_prev_state = this->state;
        }
    }

    // Clean up remote_anim if lock was released
    if (remote_anim_mumbo == this && this->state == 4) {
        remote_anim_mumbo = (Actor*)0;
    }

    // ================================================================
    // NORMAL STATE SWITCH (original logic with lock check in state 4)
    // ================================================================
    switch (this->state) {
        case 1:
            this->unk130 = func_80325760;
            chMumbo_func_802D186C(this);
            if (actor_animationIsAt(this, 0.1f) != 0) {
                FUNC_8030E624(SFX_5D_BANJO_RAAOWW, 1.0f, 6000);
            }
            if (actor_animationIsAt(this, 0.4f) != 0) {
                FUNC_8030E624(SFX_5E_BANJO_PHEWWW, 1.0f, 6000);
            }
            chMumbo_func_802D1970(this);
            break;

        case 2:
            if (actor_animationIsAt(this, 0.25f) != 0) {
                sfxsource_playHighPriority(0x41);
            }
            actor_playAnimationOnce(this);
            if (actor_animationIsAt(this, 0.999f)) {
                if (!fileProgressFlag_get(FILEPROG_11_HAS_MET_MUMBO)
                    && !volatileFlag_get(VOLATILE_FLAG_1)
                    && !volatileFlag_get(VOLATILE_FLAG_1F_IN_CHARACTER_PARADE)
                ) {
                    subaddie_set_state(this, 3);
                    gcdialog_showDialog(ASSET_D8F_DIALOG_MUMBO_MEET, 0xE, this->position, this->marker, __chMumbo_textCallback, NULL);
                    fileProgressFlag_set(FILEPROG_11_HAS_MET_MUMBO, TRUE);
                    break;
                }

                if (!fileProgressFlag_get(FILEPROG_DC_HAS_HAD_ENOUGH_TOKENS_BEFORE)
                    && !volatileFlag_get(VOLATILE_FLAG_1)
                    && !volatileFlag_get(VOLATILE_FLAG_1F_IN_CHARACTER_PARADE)
                    && this->unk38_0
                ) {
                    subaddie_set_state(this, 3);
                    gcdialog_showDialog(ASSET_DAA_DIALOG_MUMBO_HAS_ENOUGH_TOKENS, 0xE, this->position, this->marker, __chMumbo_textCallback, NULL);
                    fileProgressFlag_set(FILEPROG_DC_HAS_HAD_ENOUGH_TOKENS_BEFORE, TRUE);
                    break;
                }

                subaddie_set_state(this, 4);
            }
            break;

        case 3:
            actor_loopAnimation(this);
            break;

        case 4:
            actor_loopAnimation(this);
            sp48 = (map_get() == MAP_7A_GL_CRYPT) ? chMumbo_withinHorzDistToPlayer(0x442, 0, 0x3C) : chMumbo_withinHorzDistToPlayer(0, -0x5A, 0x3C);
            if (sp48
                && player_movementGroup() == BSGROUP_0_NONE
                && func_8028F20C()
                && func_8028EFC8()
            ) {
                // NET: Block if another player is transforming
                if (recomp_net_is_connected() && bkrecomp_net_mumbo_is_locked()
                    && !bkrecomp_net_mumbo_is_local_owner()) {
                    break;
                }

                controller_copyFaceButtons(0, face_buttons);
                if (face_buttons[FACE_BUTTON(BUTTON_B)] == 1) {
                    if (D_8037DDF0 == TRANSFORM_7_WISHWASHY) {
                        this->unk38_31 = 0;
                    } else if (player_getTransformation() == TRANSFORM_1_BANJO && !fileProgressFlag_get(__bkProgId_from_transformationId_net(D_8037DDF0)) && map_get() != MAP_7A_GL_CRYPT) {
                        this->unk38_31 = __transformation_getCost_net(D_8037DDF0);
                    }
                    this->unk38_0 = (D_8037DDF0 == TRANSFORM_7_WISHWASHY) || (item_getCount(ITEM_1C_MUMBO_TOKEN) >= this->unk38_31);
                    if (this->unk38_0) {
                        sp48 = map_get() != MAP_E_MM_MUMBOS_SKULL;
                        sp44 = player_getTransformation() == TRANSFORM_1_BANJO;
                        func_8028F94C(2, this->position);

                        // NET: Send lock before transformation starts
                        if (recomp_net_is_connected()) {
                            if (!bkrecomp_net_mumbo_send_lock()) {
                                // Lock failed — another player got it first
                                func_8028F918(0);  // release movement lock
                                break;
                            }
                            local_mumbo_actor = this;
                            local_mumbo_prev_state = this->state;
                        }

                        if (sp44 && map_get() != MAP_7A_GL_CRYPT
                            && !fileProgressFlag_get(FILEPROG_BA_HAS_SEEN_TREX_TEXT)
                            && randf() < 0.01
                            && sp48
                        ) {
                            gcdialog_showDialog(ASSET_DAE_DIALOG_MUMBO_TREX_START, 6, NULL, this->marker, __chMumbo_textCallback, NULL);
                            fileProgressFlag_set(FILEPROG_BA_HAS_SEEN_TREX_TEXT, 1);
                            this->has_met_before = TRUE;
                            subaddie_set_state(this, 3);
                        } else if (
                            sp44
                            && map_get() != MAP_7A_GL_CRYPT
                            && !this->unk138_23
                            && (sp40 = fileProgressFlag_getN(FILEPROG_BB_MUMBO_MISTAKE_INDEX, 2), sp40 < 3)
                            && randf() < 0.05
                            && sp48
                        ) {
                            this->unk138_23 = TRUE;
                            this->unk10_12 = D_8037DDF0;
                            D_8037DDF0 = 7;
                            fileProgressFlag_setN(FILEPROG_BB_MUMBO_MISTAKE_INDEX, ++sp40, 2);
                            subaddie_set_state(this, 5);
                        } else {
                            if (this->unk38_31) {
                                coMusicPlayer_playMusic(SFX_2B_BULL_MOO_1, 28000);
                                item_adjustByDiffWithHud(ITEM_1C_MUMBO_TOKEN, -this->unk38_31);
                                // NET: Broadcast token cost to all other players
                                if (recomp_net_is_connected()) {
                                    recomp_net_send_flag_change(NET_FLAG_MUMBO_ACTION, MUMBO_ACTION_DEDUCT_TOKENS, (u32)this->unk38_31, (u32)map_get());
                                }
                            }
                            subaddie_set_state(this, 5);
                        }
                        gcpausemenu_80314AC8(0);
                        break;
                    }
                    coMusicPlayer_playMusic(COMUSIC_2C_BUZZER, 22000);
                    if ((levelSpecificFlags_get(LEVEL_FLAG_3E_UNKNOWN) == FALSE) && (gcdialog_showDialog(ASSET_DAC_DIALOG_MUMBO_FAIL_TO_BUY, 0, NULL, NULL, NULL, NULL) != 0)) {
                        levelSpecificFlags_set(LEVEL_FLAG_3E_UNKNOWN, 1);
                    }
                }
            }
            break;

        case 5:
            actor_playAnimationOnce(this);
            // NET: Only apply transformation, camera, dialog when LOCAL player initiated.
            if (!recomp_net_is_connected() || local_mumbo_actor == this) {
                if (actor_animationIsAt(this, 0.35f)) {
                    sfxSource_func_8030E2C4(this->unk44_31);
                }
                if (actor_animationIsAt(this, 0.56f)) {
                    sfxSource_triggerCallbackByIndex(this->unk44_31);
                }
                if (actor_animationIsAt(this, 0.57f)) {
                    func_8030E6D4(1);
                    func_8030E6D4(1);
                }
                if (actor_animationIsAt(this, 0.01f)) {
                    comusic_playTrack(COMUSIC_1D_MUMBO_TRANSFORMATION);
                    func_8025A58C(0, 1000);
                }
                if (actor_animationIsAt(this, 0.01f)) {
                    if (this->has_met_before
                        || (this->unk10_12 == 0
                            && (player_getTransformation() != TRANSFORM_1_BANJO)
                            && (player_getTransformation() != TRANSFORM_7_WISHWASHY))
                    ) {
                        func_8028FB88(TRANSFORM_1_BANJO);
                    } else if (func_8028FB88(D_8037DDF0)) {
                        if (D_8037DDF0 != TRANSFORM_7_WISHWASHY) {
                            if (fileProgressFlag_getAndSet(__bkProgId_from_transformationId_net(D_8037DDF0), TRUE)) {
                                this->velocity[0] = 1.0f;
                            }
                            this->unk38_31 = 0;
                        }
                        if (this->unk10_12 == 1) {
                            this->unk10_12 = 0;
                        }
                    }
                }
                if (actor_animationIsAt(this, 0.79f)) {
                    func_8025A58C(-1, 1000);
                }
                if (actor_animationIsAt(this, 0.999f)) {
                    if (!this->has_met_before) {
                        func_8028F918(0);
                    }
                    func_8025A7DC(COMUSIC_1D_MUMBO_TRANSFORMATION);
                    if (player_getTransformation() != TRANSFORM_1_BANJO) {
                        subaddie_set_state(this, 3);
                        chMumbo_func_802D1B8C(this, D_8037DDF0);
                        break;
                    }
                    if (this->has_met_before) {
                        subaddie_set_state(this, 3);
                        gcdialog_showDialog(ASSET_DAF_DIALOG_MUMBO_TREX_MISTAKE, 6, NULL, this->marker, __chMumbo_textCallback, NULL);
                        break;
                    }
                    gcpausemenu_80314AC8(1);
                    subaddie_set_state(this, 4);
                }
            } else {
                // NET: Not our transformation — if we somehow got here, just wait for animation to end
                if (actor_animationIsAt(this, 0.999f)) {
                    subaddie_set_state(this, 4);
                }
            }
            break;

        case 7:
            chMumbo_func_802D186C(this);
            if (volatileFlag_get(VOLATILE_FLAG_11) == 0) {
                if (map_get() == MAP_7A_GL_CRYPT) {
                    sp48 = chMumbo_withinHorzDistToPlayer(0x453, 0, 0xBC);
                } else {
                    sp48 = chMumbo_withinHorzDistToPlayer(0, -0x6B, 0xBC);
                }
                if (sp48 != 0) {
                    gcdialog_showDialog(ASSET_DA7_DIALOG_MUMBO_CCW_SUMMER, 7, NULL, NULL, NULL, NULL);
                    volatileFlag_set(VOLATILE_FLAG_11, TRUE);
                }
            }
            actor_loopAnimation(this);
            break;

        case 8:
            chMumbo_func_802D186C(this);
            if (volatileFlag_get(VOLATILE_FLAG_12) == 0) {
                if (map_get() == MAP_7A_GL_CRYPT) {
                    sp48 = chMumbo_withinHorzDistToPlayer(0x453, 0, 0xBC);
                } else {
                    sp48 = chMumbo_withinHorzDistToPlayer(0, -0x6B, 0xBC);
                }
                if (sp48 != 0) {
                    gcdialog_showDialog(ASSET_DA8_DIALOG_MUMBO_CCW_AUTUMN, 7, NULL, NULL, NULL, NULL);
                    volatileFlag_set(VOLATILE_FLAG_12, TRUE);
                }
            }
            actor_loopAnimation(this);
            if (actor_animationIsAt(this, 0.99f)) {
                if (randf() < 0.4) {
                    temp_f12 = (randf() - 0.5) * 0.95300000000000007 * 2;
                    this->unk1C[0] = temp_f12 + ((temp_f12 >= 0.0f) ? 0.476 : -0.476);
                    subaddie_set_state_looped(this, 9);
                    break;
                }
                if (0.6 < randf()) {
                    anctrl_setDuration(this->anctrl, randf2(0.6f, 1.8f));
                }
            }
            break;

        case 9:
            this->yaw += this->unk1C[0];
            if (actor_animationIsAt(this, 0.99f)) {
                subaddie_set_state_looped(this, 8);
            }
            break;
    }
}