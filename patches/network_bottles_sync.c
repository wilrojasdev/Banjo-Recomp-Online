#include "patches.h"
#include "functions.h"
#include "enums.h"

// Network bridge
extern u32 recomp_net_is_connected(void);
extern bool bkrecomp_net_bottles_is_locked(void);
extern bool bkrecomp_net_bottles_is_local_owner(void);
extern void bkrecomp_net_bottles_send_lock(void);
extern void bkrecomp_net_bottles_send_lock_refresher(void);
extern void bkrecomp_net_bottles_send_unlock(void);
extern u8 bkrecomp_net_bottles_get_lock_owner(void);

// Decomp externs
extern enum map_e map_get(void);
extern s32 player_movementGroup(void);
extern void player_getPosition(f32 pos[3]);
extern s32 func_8028F20C(void);   // player_isStable
extern s32 func_8028F0D4(void);   // player state check
extern s32 func_8028EC04(void);   // is in special action
extern s32 func_8028EFC8(void);   // player facing check
extern s32 ability_isUnlocked(s32 ability);
extern s32 subaddie_playerIsWithinSphereAndActive(Actor *actor, s32 radius);
extern f32 nodeprop_getRadius(NodeProp *node_prop);

// From mole.c — internal functions
extern void chmole_startingDialog(Actor *this);
extern void chmole_spawnMolehill(s32 marker_as_int);
extern void func_802D9600(Actor *this);  // hide mole (state->1, invisible)
extern void func_802D9C90(Actor *this);  // free method
extern void chmole_setFacingDirection(Actor *this);
extern void chmole_learnAbility(Actor *this);

// Yaw toward arbitrary position (from code_9E370.c)
extern s32 func_803297C8(Actor *actor, f32 target_pos[3]);

// Network: get remote player position
extern u32 recomp_net_get_local_player_id(void);
typedef struct {
    f32 x, y, z;
    f32 yaw;
    u8 _rest[0x30];
} NetFullState_Pos;
extern u32 recomp_net_get_remote_state(u32 player_id, NetFullState_Pos *out);

// Mole table (from mole.c) — must match original struct layout exactly
typedef struct {
    s16 teach_text;
    s16 refresher_text;
    s8 camera_node;
    s8 ability;
} ChMoleDescription;
extern ChMoleDescription moleTable[];

// === State tracking (per-actor via pointer) ===

// The specific Bottles actor that the LOCAL player is talking to.
// NULL when no local conversation is active.
static Actor *local_talking_actor = (Actor*)0;

// Previous state of the local_talking_actor (for unlock transition detection).
static s32 local_talking_prev_state = 1;

// The specific Bottles actor showing remote animation.
// NULL when no remote animation is active.
static Actor *remote_anim_actor = (Actor*)0;

// Find the Bottles actor closest to a given position.
static Actor *find_closest_bottles(f32 pos[3]) {
    f32 dist;
    return actorArray_findClosestActorFromActorId(pos, ACTOR_37A_BOTTLES, -1, &dist);
}

// === Remote animation triggers (called from network_flag_sync.c) ===

RECOMP_EXPORT void bkrecomp_net_bottles_remote_emerge(void) {
    // Find the Bottles closest to the lock owner (the remote player who is talking)
    u8 owner_id = bkrecomp_net_bottles_get_lock_owner();
    NetFullState_Pos rs;
    if (!recomp_net_get_remote_state((u32)owner_id, &rs)) return;

    f32 owner_pos[3];
    owner_pos[0] = rs.x;
    owner_pos[1] = rs.y;
    owner_pos[2] = rs.z;

    Actor *bottles = find_closest_bottles(owner_pos);
    if (!bottles || !bottles->marker) return;

    // The host already decides whether to send LOCK (emerge) vs LOCK_REFRESHER (no emerge).
    // This function is only called for LOCK, so always show the emerge animation.
    remote_anim_actor = bottles;

    // Animate molehill partner — state 2 = opening
    Actor *molehill = subaddie_getLinkedActor(bottles);
    if (bottles->partnerActor && molehill && bottles->partnerActor->id == 0xB8) {
        subaddie_set_state_with_direction(molehill, 2, 0.0001f, 1);
    }

    // Make Bottles visible and play emerge animation
    bottles->marker->propPtr->unk8_3 = 1;
    anctrl_setSmoothTransition(bottles->anctrl, TRUE);
    subaddie_set_state_with_direction(bottles, 2, 0.0001f, 1);
    actor_playAnimationOnce(bottles);
}

RECOMP_EXPORT void bkrecomp_net_bottles_remote_hide(void) {
    if (!remote_anim_actor) return;

    Actor *bottles = remote_anim_actor;
    if (!bottles->marker) {
        remote_anim_actor = (Actor*)0;
        return;
    }

    // Animate molehill partner — state 3 = closing
    Actor *molehill = subaddie_getLinkedActor(bottles);
    if (bottles->partnerActor && molehill && bottles->partnerActor->id == 0xB8) {
        subaddie_set_state_with_direction(molehill, 3, 0.0001f, 1);
    }

    // Play exit animation — visual only
    subaddie_set_state_with_direction(bottles, 4, 0.0001f, 1);
    actor_playAnimationOnce(bottles);
}

// Helper: face Bottles toward the lock owner (ghost position)
static void face_toward_lock_owner(Actor *this) {
    u8 owner_id = bkrecomp_net_bottles_get_lock_owner();
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

// === PATCHED UPDATE ===

RECOMP_PATCH void chmole_update(Actor *this) {
    s32 sp50[6];
    f32 sp4C;
    f32 pad44[1];
    Actor *other;
    NodeProp *node_prop;
    f32 sp34[3];

    if (this->actorTypeSpecificField < 8 || this->actorTypeSpecificField >= 0x13)
        return;

    // (Bottles are found via actorArray_findClosestActorFromActorId when needed)

    // --- Init (unchanged from original) ---
    if (!this->volatile_initialized) {
        this->volatile_initialized = TRUE;
        marker_setFreeMethod(this->marker, func_802D9C90);

        // Reset stale network state for THIS actor on init
        if (local_talking_actor == this) {
            local_talking_actor = (Actor*)0;
            local_talking_prev_state = 1;
        }
        if (remote_anim_actor == this) {
            remote_anim_actor = (Actor*)0;
        }

        if (this->initialized) {
            other = actorArray_findClosestActorFromActorId(this->position, ACTOR_12C_MOLEHILL, -1, &sp4C);
            this->partnerActor = (other) ? other->marker : NULL;
            if (this->partnerActor) {
                other = subaddie_getLinkedActor(this);
                if (other && this->partnerActor->id == 0xB8) {
                    subaddie_set_state(other, 1);
                }
            }
        }
    }

    if (!this->initialized) {
        node_prop = nodeprop_findByActorIdAndActorPosition(0x372, this);
        if (node_prop == NULL) {
            this->unk38_0 = FALSE;
        } else {
            this->unk38_0 = TRUE;
            nodeprop_getPosition(node_prop, this->unk1C);
        }
        __spawnQueue_add_1((GenFunction_1)chmole_spawnMolehill, reinterpret_cast(s32, this->marker));
        this->marker->propPtr->unk8_3 = FALSE;
        this->marker->collidable = FALSE;
        this->initialized = TRUE;
        if (this->actorTypeSpecificField == 0x12) {
            node_prop = nodeprop_findByActorIdAndActorPosition(0x349, this);
            if (node_prop == NULL) {
                this->velocity[0] = this->position[0];
                this->velocity[1] = this->position[1];
                this->velocity[2] = this->position[2];
                this->actor_specific_1_f = 500.0f;
            } else {
                nodeprop_getPosition(node_prop, this->velocity);
                this->actor_specific_1_f = 2 * nodeprop_getRadius(node_prop);
            }
        }
    }

    // ================================================================
    // NET: TOP-LEVEL LOCK GUARD
    // If another player owns the lock, skip ALL normal logic for ALL
    // Bottles actors on this map. Only allow remote animation on the
    // specific actor that was triggered.
    // ================================================================
    if (recomp_net_is_connected()) {
        bool lock_is_remote = bkrecomp_net_bottles_is_locked()
                              && !bkrecomp_net_bottles_is_local_owner();

        if (lock_is_remote && !local_talking_actor) {
            // This actor is the one showing remote animation
            if (remote_anim_actor == this) {
                switch (this->state) {
                    case 2: // Emerge animation
                        this->marker->propPtr->unk8_3 = TRUE;
                        face_toward_lock_owner(this);
                        if (actor_animationIsAt(this, 0.9999f)) {
                            subaddie_set_state_with_direction(this, 3, 0.0001f, 1);
                            actor_loopAnimation(this);
                        }
                        break;
                    case 3: // Idle visible
                        face_toward_lock_owner(this);
                        break;
                    case 4: // Exit animation
                        if (actor_animationIsAt(this, 0.9999f)) {
                            func_802D9600(this);
                            remote_anim_actor = (Actor*)0;
                        }
                        break;
                    case 1: // Returned to idle
                        remote_anim_actor = (Actor*)0;
                        break;
                }
            }
            // ALL Bottles skip normal logic while remote lock is active
            return;
        }
    }

    // ================================================================
    // NET: LOCAL UNLOCK DETECTION (per-actor)
    // Only fires for the SPECIFIC Bottles actor that started the conversation.
    // Other Bottles on the same map won't trigger false unlocks.
    // ================================================================
    if (local_talking_actor == this) {
        if (this->state == 1 && local_talking_prev_state != 1) {
            bkrecomp_net_bottles_send_unlock();
            local_talking_actor = (Actor*)0;
            local_talking_prev_state = 1;
        } else {
            local_talking_prev_state = this->state;
        }
    }

    // Clean up remote_anim if lock was released and this actor was animating
    if (remote_anim_actor == this && this->state == 1) {
        remote_anim_actor = (Actor*)0;
    }

    // ================================================================
    // NORMAL (LOCAL) UPDATE
    // ================================================================
    controller_copyFaceButtons(0, sp50);

    switch (this->state) {
        case 1: // IDLE
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if (func_8028F20C() && func_8028F0D4() && !func_8028EC04()) {
                // NET: Block if another player is talking to Bottles
                if (recomp_net_is_connected() && bkrecomp_net_bottles_is_locked()
                    && !bkrecomp_net_bottles_is_local_owner()) {
                    break;
                }

                if (this->actorTypeSpecificField == 0x12
                    && !ability_isUnlocked(moleTable[this->actorTypeSpecificField - 9].ability)
                    && (player_movementGroup() == BSGROUP_0_NONE || player_movementGroup() == BSGROUP_8_TROT)
                ) {
                    player_getPosition(sp34);
                    if (ml_vec3f_distance(sp34, this->velocity) < this->actor_specific_1_f) {
                        if (recomp_net_is_connected()) {
                            bkrecomp_net_bottles_send_lock();
                            local_talking_actor = this;
                            local_talking_prev_state = this->state;
                        }
                        chmole_startingDialog(this);
                    }
                } else {
                    if (!player_movementGroup()
                        && subaddie_playerIsWithinSphereAndActive(this, 0xFA)
                        && func_8028EFC8()
                        && sp50[FACE_BUTTON(BUTTON_B)] == 1
                    ) {
                        if (recomp_net_is_connected()) {
                            // Send refresher lock if ability already learned (no emerge on remote)
                            s32 is_refresher = 0;
                            if (this->actorTypeSpecificField >= 9 && this->actorTypeSpecificField < 0x13) {
                                is_refresher = ability_isUnlocked(moleTable[this->actorTypeSpecificField - 9].ability);
                            }
                            if (is_refresher) {
                                bkrecomp_net_bottles_send_lock_refresher();
                            } else {
                                bkrecomp_net_bottles_send_lock();
                            }
                            local_talking_actor = this;
                            local_talking_prev_state = this->state;
                        }
                        chmole_startingDialog(this);
                    }
                }
            }
            break;

        case 2: // ENTER (emerge from molehill)
            this->marker->propPtr->unk8_3 = TRUE;
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if (0.0 < anctrl_getAnimTimer(this->anctrl)
                && anctrl_getAnimTimer(this->anctrl) < 0.16) {
                sfxSource_func_8030E2C4(this->unk44_31);
            }
            if (actor_animationIsAt(this, 0.9999f)) {
                chmole_setFacingDirection(this);
                sfxsource_freeSfxsourceByIndex(this->unk44_31);
                this->unk44_31 = 0;
            } else if (actor_animationIsAt(this, 0.14f)) {
                sfx_playFadeShorthandDefault(SFX_C6_SHAKING_MOUTH, 1.2f, 24000, this->position, 1250, 2500);
            } else if (actor_animationIsAt(this, 0.4f)) {
                sfx_playFadeShorthandDefault(SFX_2C_PULLING_NOISE, 1.2f, 24000, this->position, 1250, 2500);
            } else if (actor_animationIsAt(this, 0.75f)) {
                sfx_playFadeShorthandDefault(SFX_C5_TWINKLY_POP, 1.0f, 32000, this->position, 1250, 2500);
            } else if (actor_animationIsAt(this, 0.35f)) {
                chmole_learnAbility(this);
            }
            break;

        case 3: // FACING (idle visible, waiting)
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if ((actor_animationIsAt(this, 0.37f)
                  || actor_animationIsAt(this, 0.66f)
                  || actor_animationIsAt(this, 0.85f))
                && randf() < 0.2) {
                anctrl_setDirection(this->anctrl, 1 ^ anctrl_isPlayedForwards(this->anctrl));
            } else if (actor_animationIsAt(this, 0.25f)
                || actor_animationIsAt(this, 0.28f)
                || actor_animationIsAt(this, 0.31f)) {
                func_8030E878(SFX_6F_BANJO_HEADSCRATCH, randf2(1.4f, 1.55f), 16000, this->position, 1250.0f, 2500.0f);
            } else if (actor_animationIsAt(this, 0.45f)
                || actor_animationIsAt(this, 0.48f)
                || actor_animationIsAt(this, 0.51f)
                || actor_animationIsAt(this, 0.7f)
                || actor_animationIsAt(this, 0.73f)
                || actor_animationIsAt(this, 0.76f)) {
                func_8030E878(SFX_6F_BANJO_HEADSCRATCH, randf2(1.35f, 1.5f), 6000, this->position, 1250.0f, 2500.0f);
            }
            break;

        case 4: // EXIT (go back into molehill)
            if (0.35 < anctrl_getAnimTimer(this->anctrl)
                && anctrl_getAnimTimer(this->anctrl) < 0.9) {
                sfxSource_func_8030E2C4(this->unk44_31);
            } else if (actor_animationIsAt(this, 0.9999f)) {
                func_802D9600(this);
                sfxsource_freeSfxsourceByIndex(this->unk44_31);
                this->unk44_31 = 0;
            }
            break;
    }
}
