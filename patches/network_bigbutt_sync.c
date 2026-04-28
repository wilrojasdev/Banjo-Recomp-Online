/* Bigbutt (MM) state-machine sync — Conga pattern.
 *
 * Vanilla func_802C6240 (chBigbuttUpdate) runs the full state machine on
 * every machine. In multiplayer, even with position+anim_id replicated, the
 * non-owner's local state machine resets the animation on every
 * subaddie_set_state* call and picks state transitions independently
 * (sphere checks fire on different frames, randf diverges, etc.), so the
 * remote view drifts visibly from the host.
 *
 * Following the Conga model (network_conga_sync.c), the non-owner skips the
 * state machine entirely. The synced fields drive the visuals:
 *   - actor->state                (network_world_sync.c is_synced_special_actor)
 *   - anim_id + anim_timer + dir  (committed via _anctrl_start in the same path)
 *   - position + yaw              (interpolated)
 *
 * What still runs on non-owner:
 *   1. The init block (registers dieFunc / collisionFunc / collision2Func
 *      so collisions still hurt local Banjo and the dieFunc is later
 *      proxied by network_world_sync.c).
 *   2. The marker->id flip between BIGBUTT_MARKER_ALIVE (0x3) and
 *      BIGBUTT_MARKER_FALLEN (0x29e). Required so the bulk-position matcher
 *      can find this actor regardless of whether the host is alive (0x3)
 *      or fallen (0x29e). The world-sync matcher also aliases the two ids.
 *
 * AI chase: not handled here. The dispatcher patch in network_enemy_ai.c
 * already redirects player_getPosition to the closest player while a
 * killable enemy update is running, so the owner's vanilla switch picks
 * remote players as targets via subaddie_playerIsWithinSphereAndActive
 * and func_80329784.
 */

#include "patches.h"
#include "functions.h"
#include "variables.h"
#include "enums.h"
#include "core2/anctrl.h"

/* === Network bridges === */
extern u32 recomp_net_is_connected(void);
extern u32 recomp_net_am_i_world_owner(u32 level_id);
extern u32 bkrecomp_net_am_i_actor_owner(Actor *actor);
extern enum level_e level_get(void);

/* === Vanilla helpers in core2/ch/bigbutt.c (file-global, not in headers) === */
extern void func_802C5E80(Actor *this);
extern void func_802C5EB8(Actor *this);
extern void func_802C5F44(Actor *this);
extern void func_802C5F94(Actor *this);
extern void func_802C5FF8(Actor *this);
extern void func_802C60AC(ActorMarker *marker, ActorMarker *other);
extern void func_802C6150(ActorMarker *marker, ActorMarker *other);
extern void func_802C61C0(ActorMarker *marker, ActorMarker *other);

/* === Other engine externs not in functions.h === */
extern s32  subaddie_playerIsWithinSphereAndActive(Actor *, s32);
extern s32  globalTimer_getTime(void);
extern s32  player_getWaterState(void);
extern s32  func_803292E0(Actor *);
extern s32  func_803294B4(Actor *, s32);
extern s32  func_803294F0(Actor *, s32, s32);

#define BIGBUTT_MARKER_ALIVE   0x3
#define BIGBUTT_MARKER_FALLEN  0x29e
#define BIGBUTT_STATE_FALLEN   0xe

/* Replicated vanilla func_802C6240 with a non-owner fast path. */
RECOMP_PATCH void func_802C6240(Actor *this) {
    s32 sp2C;
    u8  tmp_a0;
    f32 tmp_f0;
    bool is_non_owner;

    if (!this->initialized) {
        this->marker->dieFunc = func_802C61C0;
        this->marker->collisionFunc = func_802C60AC;
        this->marker->collision2Func = func_802C6150;
        this->has_met_before = FALSE;
        this->unk16C_0 = 1;
        this->initialized = TRUE;
        return;
    }

    /* Per-actor dynamic ownership: only skip the state machine when SOMEONE
     * ELSE is closer to this Bigbutt and broadcasting authoritative state.
     * If I'm the closest player (e.g., host is far away on the other side
     * of the map), I run the vanilla state machine locally — its aggro and
     * yaw helpers see my own Banjo as the closest player via the
     * _player_getPosition / func_80329784 redirects in network_enemy_ai.c. */
    is_non_owner = recomp_net_is_connected()
                && !bkrecomp_net_am_i_actor_owner(this);

    if (is_non_owner) {
        /* Skip the state machine entirely on non-owner. The synced
         * actor->state and anim_id (committed via _anctrl_start in
         * network_world_sync.c is_synced_special_actor path) drive the
         * visuals. Just keep the marker->id flip so bulk-position
         * matching keeps finding this actor across the alive/fallen
         * boundary. */
        if (this->state == BIGBUTT_STATE_FALLEN) {
            if (this->marker->id != BIGBUTT_MARKER_FALLEN)
                this->marker->id = BIGBUTT_MARKER_FALLEN;
        } else {
            if (this->marker->id != BIGBUTT_MARKER_ALIVE)
                this->marker->id = BIGBUTT_MARKER_ALIVE;
        }
        return;
    }

    /* Owner / single-player: vanilla state machine.
     * Mirrors lib/bk-decomp/src/core2/ch/bigbutt.c func_802C6240 verbatim,
     * with D_80366010[N] replaced by this->unk18[N] (the same
     * ActorAnimationInfo table reached through the actor). */
    switch (this->state) {
        case 0x1:
            this->unk10_12 = 3;
            sp2C = func_8032863C(this->anctrl, 0.16f, 0.55f);
            if (!this->unk138_28) {
                if (actor_animationIsAt(this, 0.157f)
                    || actor_animationIsAt(this, 0.289f)
                    || actor_animationIsAt(this, 0.4f)
                    || actor_animationIsAt(this, 0.536f)) {
                    if (player_getWaterState() != BSWATERGROUP_2_UNDERWATER) {
                        func_8030E878(SFX_C8_CRUNCH, randf2(0.93f, 1.07f), 10000, this->position, 0.0f, 1800.0f);
                    }
                }
            }
            if (sp2C == 2) {
                func_80328A2C(this, 0.55f, 1, 0.35f);
            }
            func_802C5FF8(this);
            if (func_8032863C(this->anctrl, 0.65f, 0.99f) >= 2
                && !func_80328A2C(this, 0.0f, -1, 0.45f)
                && subaddie_maybe_set_state_position_direction(this, 2, 0.0f, -1, 0.58f)) {
                func_80328CEC(this, (s32)this->yaw, 10, 45);
                func_802C5E80(this);
            }
            func_802C5EB8(this);
            break;

        case 0x2:
            func_802C5FF8(this);
            func_80328FB0(this, 2.0f);
            if (!func_80329030(this, 0) && func_80329480(this)) {
                func_80328CEC(this, (s32)this->yaw, 90, 150);
            }
            if (!(globalTimer_getTime() & 0xf))
                func_80328CEC(this, (s32)this->yaw_ideal, 10, 20);

            if (!(globalTimer_getTime() & 0x7))
                subaddie_maybe_set_state_position_direction(this, 1, 0.16f, 1, 0.02f);

            if (!(globalTimer_getTime() & 0xf)
                && func_80329078(this, (s32)this->yaw_ideal, 150)
                && subaddie_maybe_set_state(this, 3, 0.13f)) {
                this->actor_specific_1_f = randf2(7.1f, 8.4f);
            }
            func_802C5EB8(this);
            break;

        case 0x8:
            func_802C5F44(this);
            this->yaw_ideal = func_80329784(this);
            func_80328FB0(this, 4.0f);
            if (func_80329480(this))
                subaddie_set_state(this, 6);
            break;

        case 0x3:
            func_80328FB0(this, 3.0f);
            if (!func_80329030(this, 0) && func_80329480(this)) {
                func_80328CEC(this, (s32)this->yaw, 120, 180);
                subaddie_set_state(this, 2);
                func_802C5E80(this);
            }
            if (!(globalTimer_getTime() & 0xf) && subaddie_maybe_set_state(this, 2, 0.08f))
                func_802C5E80(this);
            func_802C5EB8(this);
            break;

        case 0x6:
            anctrl_setDuration(this->anctrl, this->unk18[6].duration - (3 - this->unk10_12) * 0.1085f);
            this->yaw_ideal = (f32)func_80329784(this);
            if (!func_803294B4(this, 0x21)) {
                subaddie_set_state(this, 8);
            }
            func_802C5F44(this);
            if (actor_animationIsAt(this, 0.35f) && player_getWaterState() != BSWATERGROUP_2_UNDERWATER) {
                func_8030E58C(SFX_3C_BULL_GROWN, randf() / 10.0f + 1.0f);
                this->unk10_12--;
            }
            if (!func_80329078(this, (s32)this->yaw, 20))
                func_802C5F94(this);

            if (this->unk10_12 == 0
                || (this->unk10_12 < 3 && subaddie_playerIsWithinSphereAndActive(this, 300))) {
                subaddie_set_state(this, 9);
                this->actor_specific_1_f = 13.0f;
            }
            break;

        case 0x9:
            if (actor_animationIsAt(this, 0.35f))
                func_8030E58C(SFX_2E_BIGBUTT_RUNNING, 1.0f);

            this->actor_specific_1_f += 0.15f;
            if (30.0f < this->actor_specific_1_f) {
                this->actor_specific_1_f = 30.0f;
            }

            this->yaw_ideal = (f32)func_80329784(this);
            func_80328FB0(this, 9.0f);
            if (!func_80329030(this, 0))
                func_802C5F94(this);

            if (subaddie_playerIsWithinSphereAndActive(this, 320)) {
                if (func_80329078(this, (s32)this->yaw_ideal, 200)) {
                    anctrl_setPlaybackType(this->anctrl, ANIMCTRL_ONCE);
                    subaddie_set_state(this, 4);
                    this->actor_specific_1_f += 5.7f;
                    tmp_a0 = this->unk44_31;
                    if (this->unk44_31 == 0) {
                        this->unk44_31 = sfxsource_createSfxsourceAndReturnIndex();
                        tmp_a0 = this->unk44_31;
                    }
                    sfxsource_setSfxId(tmp_a0, SFX_18_BIGBUTT_SLIDE);
                    sfxSource_setunk43_7ByIndex(this->unk44_31, 2);
                    sfxsource_playSfxAtVolume(this->unk44_31, (randf() * 0.1f - 0.05f) + 1.0f);
                    sfxSource_func_8030E2C4(this->unk44_31);
                } else {
                    func_802C5F94(this);
                }
            }
            break;

        case 0x4:
            if (anctrl_getAnimTimer(this->anctrl) < 0.99f) {
                this->yaw_ideal = (f32)func_80329784(this);
                func_80328FB0(this, 1.0f);
            }
            func_80329030(this, 0);
            sfxSource_func_8030E2C4(this->unk44_31);
            if (0.99f <= anctrl_getAnimTimer(this->anctrl)) {
                func_80329878(this, subaddie_playerIsWithinSphereAndActive(this, 250) ? 0.8f : 1.2f);
                if (0.0f == this->actor_specific_1_f) {
                    anctrl_setPlaybackType(this->anctrl, ANIMCTRL_LOOP);
                    subaddie_set_state_with_direction(this, 1, 0.65f, 1);
                    sfxsource_freeSfxsourceByIndex(this->unk44_31);
                    this->unk44_31 = 0;
                    sfxsource_playHighPriority(SFX_19_BANJO_LANDING_08);
                }
            }
            break;

        case 0x5:
            actor_playAnimationOnce(this);
            tmp_f0 = anctrl_getAnimTimer(this->anctrl);
            anctrl_setDuration(this->anctrl, this->unk18[5].duration + ((0.65f < tmp_f0) ? (tmp_f0 - 0.65f) * 16.0f : 0.0f));
            if (actor_animationIsAt(this, 0.95f)) {
                actor_loopAnimation(this);
                func_802C5F94(this);
            }
            break;

        case 0xc:
            actor_playAnimationOnce(this);
            if (actor_animationIsAt(this, 0.95f)) {
                subaddie_set_state_with_direction(this, 1, 0.65f, 1);
                actor_loopAnimation(this);
            }
            break;

        case 0xd:
            actor_playAnimationOnce(this);
            if (actor_animationIsAt(this, 0.95f)) {
                subaddie_set_state_with_direction(this, 0xe, 0.99f, 1);
                this->lifetime_value = 4.0f;
            }
            break;

        case 0xe:
            actor_playAnimationOnce(this);
            this->lifetime_value -= time_getDelta();
            if (this->lifetime_value <= 0.0f) {
                this->unk166 = 0x63;
                subaddie_set_state_forward(this, 0xF);
            }
            break;

        case 0xf:
            actor_playAnimationOnce(this);
            if (actor_animationIsAt(this, 0.95f)) {
                subaddie_set_state_with_direction(this, 1, 0.65f, 1);
                actor_loopAnimation(this);
            }
            break;
    }

    if (this->state == BIGBUTT_STATE_FALLEN) {
        if (this->marker->id != BIGBUTT_MARKER_FALLEN)
            this->marker->id = BIGBUTT_MARKER_FALLEN;
    } else {
        if (this->marker->id != BIGBUTT_MARKER_ALIVE)
            this->marker->id = BIGBUTT_MARKER_ALIVE;
    }
}
