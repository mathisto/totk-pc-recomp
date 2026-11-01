// Code mods ported as hooks (recomp/patches.py `hook`): each callback runs in the lifted C ahead of a guest
// instruction, with the Cpu current, as an exlaunch inline hook's callback runs on the console with the hook context.
//
// Set tsq (recomp/patches-v121.txt): the code half of the mod "The Second Quest" for 1.2.1. On the console it is an
// exlaunch module, "MSFix" (exefs/subsdk9, build id 8a05c481...), that installs seven inline hooks on main, plus an
// IPS (recomp/patches-v121.txt: one site and one data word). The behaviour below was read from that module's
// instructions and the 1.2.1 symbol names (dt-12345/totk_syms); the fixes are by dt12345 (Compendium crash, vanilla
// Master Sword), Fruithapje21 (stamina refill delay) and VyraNightingale (Mineru durability cap), credited in the mod. The module keeps the inline hook context
// d0-d15 at +0x00 and x0-x29 at +0x80, and restores all of it after a callback, which is why a callback that calls
// guest code puts the registers back (restore_regs): only what the original callback writes into the context changes.
#include <stdlib.h>
#include <string.h>

#include "host.h"

#if __has_include("patches.h")
#include "layout.h"
#include "patches.h"

#define MAIN(off) (MOD_MAIN_BASE + (off))

// One log line the first time each callback runs, so a session's log shows which hooks the game reached.
#define FIRST_RUN(what)                                                                     \
    do {                                                                                    \
        static int ran;                                                                     \
        if (!__atomic_exchange_n(&ran, 1, __ATOMIC_RELAXED)) rt_log("hooks: %s first ran", what); \
    } while (0)

// The registers an exlaunch callback's context restores, put back after guest code a callback called: x0-x30, sp,
// the flags and the vector registers. The thread's other state (exclusive monitor, TLS, FPCR) stays as the call left
// it.
static void restore_regs(Cpu *c, const Cpu *s) {
    memcpy(c->x, s->x, sizeof c->x);
    c->sp = s->sp;
    c->pc = s->pc;
    c->nzcv = s->nzcv;
    memcpy(c->v, s->v, sizeof c->v);
}
// The stamina delay timer, two floats {value, reset value}, in guest memory because guest code steps it: the patch
// page, +256..263 (recomp/patches.py lists the page's words).
#define TSQ_TIMER (RT_PATCH_PAGE + 0x100)

// main+0x198c59c and +0x198c6c0, each a `bl main+0x80eb0c` in a loop of game::scene::ResurrectMasterSword (vanilla
// Master Sword fix): before the call, the same function is called once more with a fixed key 0xEE199C3A, the value -1
// passed by pointer and the loop's x0 and x2, as MSFix's callback does from its own stack frame.
void tsq_sword_resurrect(Cpu *c) {
    FIRST_RUN("tsq_sword_resurrect");
    Cpu saved = *c;
    c->sp -= 0x20;
    ST32(c->sp + 0x1c, 0xFFFFFFFFu);
    rt_call(c, MAIN(0x80eb0c), 4, (uint64_t)saved.x[0], (uint64_t)(c->sp + 0x1c), (uint64_t)saved.x[2],
            (uint64_t)0xEE199C3Au);
    restore_regs(c, &saved);
}

// main+0x66589c, `str w8, [x19, #0x4c8]` after a clamp in game::component::DynamicEquipment::preCalc (vanilla Master
// Sword fix): when the actor at [x19 + 0x18]
// is Weapon_Sword_070, w8 also goes to [[[actor + 0x228] + 0x2f8] + 0x1840] + 8, each pointer checked. (MSFix steps
// the actor back 8 bytes on 1.0.0 only.)
void tsq_sword_equip(Cpu *c) {
    FIRST_RUN("tsq_sword_equip");
    uint64_t actor = LD64(c->x[19] + 0x18);
    if (!actor) return;
    uint64_t name = LD64(actor + 0x218);
    if (!name || strcmp((const char *)(uintptr_t)name, "Weapon_Sword_070")) return;
    uint64_t p = LD64(actor + 0x228);
    if (p) p = LD64(p + 0x2f8);
    if (p) p = LD64(p + 0x1840);
    if (p) ST32(p + 8, (uint32_t)c->x[8]);
}

// main+0x1d53d98, +0x1d54774 and +0x1d85a64, in the player's charge attack, charged long-sword spin and special
// attack (game::ai::execute::ExecutePlayer*::updateImpl_; stamina refill delay): those attacks arm the timer at 90.
void tsq_stamina_arm(Cpu *c) {
    FIRST_RUN("tsq_stamina_arm");
    (void)c;
    float *t = (float *)(uintptr_t)TSQ_TIMER;
    t[0] = t[1] = 90.0f;
}

// main+0x9f1cb0, `str s1, [x19, #0x74]` in game::player::StaminaCalculator::calcRecovery, the refill and clamp: while the timer runs, it is stepped by the
// game's own timer step (main+0xec98cc, VFRCounter::countup) and s0 becomes the stamina as it stands, so the refill
// waits; once it has run out it is cleared. As MSFix writes only the low 32 bits of the d0 slot and its context
// restore loads d0, the rest of d0 stays and the upper half of v0 reads zero.
void tsq_stamina_hold(Cpu *c) {
    FIRST_RUN("tsq_stamina_hold");
    float *t = (float *)(uintptr_t)TSQ_TIMER;
    if (!(t[0] > 0.0f)) {
        t[0] = t[1] = 0.0f;
        return;
    }
    Cpu saved = *c;
    rt_call(c, MAIN(0xec98cc), 1, (uint64_t)TSQ_TIMER);
    restore_regs(c, &saved);
    uint64_t d0 = ((uint64_t)c->v[0] & 0xFFFFFFFF00000000ull) | LD32(c->x[19] + 0x74);
    c->v[0] = d0;
}

// Set deflect: Elden Ring Reforged's two deflect tiers on TotK's guard. A guard (ZL) pressed shortly before a hit lands is
// a deflect; pressed just before it, a perfect deflect, as a well-timed parry is.
//
// - Where the game decides. game::sensor::CalcGuardFlags (main+0xa093d8) runs when a contact is registered and returns
//   bit0 guarded, bit1 just guard (only while the shield's JustGuard animation event is on) and bit2 dodge.
//   HitInfoBase::setHitInfo_ stores the result in the hit record of both sides.
// - A perfect deflect sets bit1 there. Everything after that follows natively: GuardType 6, the attacker's recoil, and
//   unparryable attacks still win. Natively only the parry action plays the player's side
//   (game::actor::GameActor::calcGuardJustSuccess, main+0x1724f10: slow motion, sparks, rumble), so after a forced one
//   the second hook calls it.
// - A deflect makes game::life::GuardCalculator::calcGuardReaction answer GuardSuccessRepelAttacker (type 4: the
//   attacker is pushed back) where it would answer a plain GuardSuccess. Guard breaks and attacks that ignore a guard
//   keep their answer.
// - Anti-mash (ERR): a press within deflect_mash_ms of the one before turns the perfect tier off for deflect_lock_ms;
//   deflects still work. Any deflect ends the lock (Sekiro: a success clears the penalty).
// - Flurries (ERR): keep holding guard after a deflect, and each hit within deflect_flurry_ms of the last one gets the
//   same tier.
//
// Notes: totk-deflect-research/re/REPORT.md and frame-ratios.md in the author's scratch.
//
// totk.ini (all but deflect apply live):
// - deflect = 1 (read at start);
// - deflect_perfect_ms 130 and deflect_window_ms 330: ERR's 100 and 300 ms plus about one 30 fps frame, because a press
//   is timed from the frame the game read it to the frame the contact registered;
// - deflect_mash_ms 730, deflect_lock_ms 1200, deflect_flurry_ms 730;
// - deflect_log = 1: one line per decision.
#include "cfg.h"
#include "input.h"
#include "platform/platform.h"

static struct { unsigned ver; uint64_t perfect_ns, window_ns, mash_ns, lock_ns, flurry_ns; int log; } dfl;

static uint64_t cfg_ms(const char *name, int def) {
    const char *s = cfg_get(name);
    return (uint64_t)(s && *s ? atoi(s) : def) * 1000000u;
}

static void deflect_cfg(void) {
    unsigned v = cfg_version() + 1;   // + 1: version 0 is the first parse
    if (__atomic_load_n(&dfl.ver, __ATOMIC_ACQUIRE) == v) return;
    dfl.perfect_ns = cfg_ms("TOTK_DEFLECT_PERFECT_MS", 130);
    dfl.window_ns = cfg_ms("TOTK_DEFLECT_WINDOW_MS", 330);
    dfl.mash_ns = cfg_ms("TOTK_DEFLECT_MASH_MS", 730);
    dfl.lock_ns = cfg_ms("TOTK_DEFLECT_LOCK_MS", 1200);
    dfl.flurry_ns = cfg_ms("TOTK_DEFLECT_FLURRY_MS", 730);
    const char *l = cfg_get("TOTK_DEFLECT_LOG");
    dfl.log = l && *l && strcmp(l, "0");
    __atomic_store_n(&dfl.ver, v, __ATOMIC_RELEASE);
}

enum { TIER_GUARD, TIER_DEFLECT, TIER_PERFECT };
static const char *const tier_name[] = {"guard", "deflect", "perfect"};

// The two records of one contact (the attacker's and the defender's) are stored by separate setHitInfo_ calls, a
// frame apart or on different threads. The first call decides; the second, and calcGuardReaction, read the decision.
// Both are keyed by the attack id, and by the attacker where it is known.
typedef struct { uint64_t atk, id, ns; int tier; } DeflectHit;
static DeflectHit dfl_hits[16];
static unsigned dfl_next;
static PlatMutex dfl_lock;
#define DFL_HIT_NS 1000000000ull   // a decision is kept a second

// Under dfl_lock: the last deflect of either tier (its press and when it landed). The same moment also forgives any
// mashing before it.
static uint64_t dfl_last_press, dfl_last_ns;
static int dfl_last_tier;

// The player actor, as deflect_flags last saw it.
static uint64_t dfl_player;

// The forced deflect the player side still has to play: the defender and when.
static uint64_t dfl_pending_actor, dfl_pending_ns;

static int is_player(uint64_t actor) {
    if (!actor) return 0;
    uint64_t name = LD64(actor + 0x218);
    return name && !strcmp((const char *)(uintptr_t)name, "Player");
}

// The tier of a hit landing at now, under dfl_lock. *press: when guard last went down (0 = never); *locked: whether
// mashing took the perfect tier.
static int deflect_tier(uint64_t now, uint64_t *press, int *locked) {
    uint64_t t[8];
    int held, n = input_guard_presses(t, 8, &held);
    *press = n ? t[0] : 0;
    *locked = 0;
    if (!n) return TIER_GUARD;
    // Holding through a flurry: the same press, still held, the last deflect recent.
    if (held && dfl_last_press == t[0] && dfl_last_tier != TIER_GUARD && now - dfl_last_ns <= dfl.flurry_ns)
        return dfl_last_tier;
    // Mashing: a press within mash_ns of the one before locks the perfect tier until lock_ns after it, unless a deflect
    // landed after it.
    for (int i = 0; i + 1 < n; i++)
        if (t[i] - t[i + 1] < dfl.mash_ns && now - t[i] < dfl.lock_ns && t[i] > dfl_last_ns) *locked = 1;
    uint64_t age = now - t[0];
    if (age <= dfl.perfect_ns && !*locked) return TIER_PERFECT;
    return age <= dfl.window_ns ? TIER_DEFLECT : TIER_GUARD;
}

// main+0x14df158, `strb w0, [x20, #0xb]` in game::sensor::HitInfoBase::setHitInfo_, right after `bl CalcGuardFlags`:
// w0 the flags, [x29 - 0x40] the defender, [x29 - 0x30] the attacker, the attack id at x20 + 0x38.
void deflect_flags(Cpu *c) {
    FIRST_RUN("deflect_flags");
    uint32_t flags = (uint32_t)c->x[0] & 0xff;
    if ((flags & 3) != 1) return;   // not guarded, or already a perfect guard
    uint64_t def = LD64(c->x[29] - 0x40), atk = LD64(c->x[29] - 0x30);
    if (!is_player(def)) return;
    __atomic_store_n(&dfl_player, def, __ATOMIC_RELAXED);
    deflect_cfg();
    uint64_t id = (uint64_t)LD32(c->x[20] + 0x3c) << 32 | LD32(c->x[20] + 0x38), now = plat_now_ns(), press = 0;
    int tier = -1, locked = 0;
    plat_mutex_lock(&dfl_lock);
    for (int i = 0; i < 16; i++)
        if (dfl_hits[i].atk == atk && dfl_hits[i].id == id && now - dfl_hits[i].ns < DFL_HIT_NS) tier = dfl_hits[i].tier;
    int first = tier < 0;
    if (first) {
        tier = deflect_tier(now, &press, &locked);
        dfl_hits[dfl_next++ % 16] = (DeflectHit){atk, id, now, tier};
        if (tier != TIER_GUARD) {
            dfl_last_press = press;
            dfl_last_ns = now;
            dfl_last_tier = tier;
        }
    }
    plat_mutex_unlock(&dfl_lock);
    if (first && dfl.log)
        rt_log("deflect: hit %#llx/%#llx press %lld ms before%s -> %s", (unsigned long long)atk, (unsigned long long)id,
               press ? (long long)((now - press) / 1000000u) : -1LL, locked ? " (mash lock)" : "", tier_name[tier]);
    if (tier != TIER_PERFECT) return;
    c->x[0] |= 2;
    if (first) {
        __atomic_store_n(&dfl_pending_ns, now, __ATOMIC_RELAXED);
        __atomic_store_n(&dfl_pending_actor, def, __ATOMIC_RELEASE);
    }
}

// main+0x6515cc, the entry of game::component::Player::calc (the phase the parry action plays in): while a forced
// deflect is pending, the native player side, until it reports done or half a second has passed. Not when A went down
// around the hit: the parry action is then playing it itself.
void deflect_effects(Cpu *c) {
    uint64_t def = __atomic_load_n(&dfl_pending_actor, __ATOMIC_ACQUIRE);
    if (!def) return;
    FIRST_RUN("deflect_effects");
    uint64_t at = __atomic_load_n(&dfl_pending_ns, __ATOMIC_RELAXED), now = plat_now_ns(), a = input_a_press();
    if (now - at > 500000000ull || (a && a + 300000000ull > at)) {
        __atomic_compare_exchange_n(&dfl_pending_actor, &def, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        return;
    }
    Cpu saved = *c;
    uint64_t done = rt_call(c, MAIN(0x1724f10), 1, def);
    restore_regs(c, &saved);
    if (done & 1) {
        __atomic_compare_exchange_n(&dfl_pending_actor, &def, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        deflect_cfg();
        if (dfl.log) rt_log("deflect: player side played after %lld ms", (long long)((now - at) / 1000000u));
    }
}

// main+0xa077e8, `tbz w0, #0` in game::life::GuardCalculator::calcGuardReaction, after the bl to
// ExternalDamageParameters::isAttackRepelled (main+0x16ff904). w0 bit0 answers RepelAttacker (4), otherwise
// GuardSuccess (3). x23 is the GuardCalculator ([+0x78] its actor), x21 the guard info (the attack id at +0, copied
// from the hit record's +0x38). A deflect decided for that hit on the player makes it a repel.
void deflect_repel(Cpu *c) {
    if (c->x[0] & 1) return;
    if (!is_player(LD64(c->x[23] + 0x78))) return;
    FIRST_RUN("deflect_repel");
    uint64_t id = LD64(c->x[21]), now = plat_now_ns();
    int tier = TIER_GUARD;
    plat_mutex_lock(&dfl_lock);
    for (int i = 0; i < 16; i++)
        if (dfl_hits[i].id == id && now - dfl_hits[i].ns < DFL_HIT_NS) tier = dfl_hits[i].tier;
    plat_mutex_unlock(&dfl_lock);
    if (tier != TIER_DEFLECT) return;
    c->x[0] |= 1;
    deflect_cfg();
    if (dfl.log) rt_log("deflect: %#llx repelled", (unsigned long long)id);
}

// main+0xa07800, `ldp x20, x19, [sp, #0x50]`: the epilogue of calcGuardReaction, which every answer passes. x19 is the
// guard result (GuardType at +0x58, flags at +0x72), x21 and x23 as in deflect_repel.
// - A deflect also turns a guard break (5: the shield knocked aside, the player staggered) into a repel, as ERR's
//   deflect boosts the guard. That covers the break from the strong attack attributes (mask 0x2000030), and the shield
//   drop it may carry (+0x72 bit2) goes with it.
// - The break an attack always deals (attribute bit47, which sets +0x72 bit0) stays: ERR keeps attacks that cannot be
//   deflected.
// - deflect_log: the game's answer to every guarded hit on the player.
void deflect_reaction(Cpu *c) {
    if (!is_player(LD64(c->x[23] + 0x78))) return;
    FIRST_RUN("deflect_reaction");
    uint64_t id = LD64(c->x[21]), now = plat_now_ns();
    int tier = -1;
    plat_mutex_lock(&dfl_lock);
    for (int i = 0; i < 16; i++)
        if (dfl_hits[i].id == id && now - dfl_hits[i].ns < DFL_HIT_NS) tier = dfl_hits[i].tier;
    plat_mutex_unlock(&dfl_lock);
    if (tier < 0) return;   // not a guarded hit the contact hook saw
    uint32_t type = LD32(c->x[19] + 0x58);
    uint8_t flags = LD8(c->x[19] + 0x72);
    int broken = tier == TIER_DEFLECT && type == 5 && !(flags & 1);
    if (broken) {
        ST32(c->x[19] + 0x58, 4);
        ST8(c->x[19] + 0x72, flags & ~4u);
    }
    deflect_cfg();
    if (dfl.log)
        rt_log("deflect: %#llx answer %u (+0x72 %#x) for %s%s", (unsigned long long)id, type, flags, tier_name[tier],
               broken ? ": guard break -> repel" : "");
}

// No shield wear on a deflect (ERR: a deflect costs the shield nothing). A blocked hit costs the player no health in
// TotK, only the shield's durability.
// - game::life::DamageCalculatorExtensionEquipment::calcCustomInternalDamage (main+0x1177264) is the shield's own
//   damage. While its owner guards, it reads the owner's guard result (x24, the owner's components +0x2f8; GuardType at
//   +0x100) and wears the shield, by DamageGlobalParam's shield rate, only for types 1, 3, 4 and 5. A perfect guard (6)
//   costs nothing natively.
// - The hook sits on that test (main+0x11776b0, `cmp w8, #5`, w8 the type, x22 the owner's EquipmentUser) and answers
//   6 for the player's shield when the guard result is an attack decided a deflect of either tier. The record's attack
//   id is at +0xf4, which the function itself compares a little later (verified in game: every guarded hit's id).
// - Fire and shock through a shield (Chemical::onGuardHit) still apply.
// - deflect_log: one line per guard result the test sees.
void deflect_wear(Cpu *c) {
    uint32_t type = (uint32_t)c->x[8];
    if (type > 5 || !(0x3au >> type & 1)) return;
    uint64_t player = __atomic_load_n(&dfl_player, __ATOMIC_RELAXED), comps = player ? LD64(player + 0x228) : 0;
    if (!comps || c->x[22] != LD64(comps + 0x230)) return;
    FIRST_RUN("deflect_wear");
    uint64_t rec = c->x[24], id = (uint64_t)LD32(rec + 0xf8) << 32 | LD32(rec + 0xf4), now = plat_now_ns();
    int tier = -1;
    plat_mutex_lock(&dfl_lock);
    for (int i = 0; id && i < 16; i++)
        if (dfl_hits[i].id == id && now - dfl_hits[i].ns < DFL_HIT_NS) tier = dfl_hits[i].tier;
    plat_mutex_unlock(&dfl_lock);
    int cancel = tier == TIER_DEFLECT || tier == TIER_PERFECT;
    if (cancel) c->x[8] = 6;
    deflect_cfg();
    // One line per hit and answer: the test can run again for the same guard result.
    static uint64_t logged;
    uint64_t key = id ^ (uint64_t)type << 56;
    if (dfl.log && __atomic_exchange_n(&logged, key, __ATOMIC_RELAXED) != key)
        rt_log("deflect: shield wear for %#llx (type %u, %s): %s", (unsigned long long)id, type,
               tier < 0 ? "no decision" : tier_name[tier], cancel ? "cancelled" : "kept");
}

// Set lockon: Elden Ring style toggle lock-on, so guard (ZL) taps for the deflect no longer drop the lock.
// - Natively, game::ai::execute::ExecutePlayerCalcKey::updateImpl_ turns a held ZL into a lock-on request every frame
//   (the player's AttentionReceiver +0x194 |= 0x10), and the lock holds only while the request keeps coming.
// - The hook makes our toggle, not ZL, the request. The lock-on button is the game's RCLICK, which input.c hides from
//   the game.
// - Turning on locks onto the game's own best target, turning off releases. With nothing to lock onto, or once the
//   target is gone, the toggle switches itself off. A lock the game makes itself (Flurry Rush) turns it on.
// - A right-stick flick while locked drops the request for one frame. That is the game's own target switch: release
//   and press again within its window, and the next candidate by its score.
// - ZL still reaches the game, so guard, perfect guard, side-hop and backflip are unchanged.
// - Natively a lock implies a held ZL, so the shield posture follows the lock: the shield is raised toward the target
//   for as long as the lock lasts. A second hook makes it follow ZL, as Elden Ring's lock-on leaves the guard down.
// Notes: totk-deflect-research/re/LOCKON.md in the author's scratch.
// totk.ini: lockon = 1 (read at start); totk.ini's bind_RCLICK picks the button.

// Only the CalcKey thread touches this.
static struct { int init, on, released, drop, frames; unsigned presses, flicks; } lk;
#define LK_LOST_FRAMES 10   // frames without a lock before the toggle gives up on it

// main+0x1d31020, `tbnz w22, #2` in ExecutePlayerCalcKey::updateImpl_: w22 the hold mask in sead button bits (ZL is
// bit 2), already stored to the blackboard and only read by this branch from here; x19 the player's AttentionReceiver
// (+0xe8 its lock state: 0 idle, 1 the target-switch window, 2 starting, 3 locked).
void lockon_request(Cpu *c) {
    FIRST_RUN("lockon_request");
    uint64_t r = c->x[19];
    uint32_t state = r ? LD32(r + 0xe8) : 0;
    int locked = (state & ~1u) == 2;
    unsigned p, f;
    input_lockon_counts(&p, &f);
    if (!lk.init) {
        lk.init = 1;
        lk.presses = p;
        lk.flicks = f;
    }
    lk.frames++;
    if (p != lk.presses) {
        lk.presses = p;
        lk.on = !locked;
        lk.released = locked;
        lk.drop = 0;
        lk.frames = 0;
        rt_log("lockon: %s (state %u)", lk.on ? "on" : "off", state);
    }
    if (f != lk.flicks) {
        lk.flicks = f;
        if (lk.on && locked) {
            lk.drop = 1;
            lk.frames = 0;
            rt_log("lockon: switch target");
        }
    }
    if (!locked) lk.released = 0;
    if (!lk.on && locked && !lk.released) {
        lk.on = 1;
        lk.frames = 0;
        rt_log("lockon: the game locked on (state %u), toggle on", state);
    } else if (lk.on && !locked && lk.frames >= LK_LOST_FRAMES) {
        lk.on = 0;
        rt_log("lockon: no target (state %u), toggle off", state);
    }
    int request = lk.on && !lk.drop;
    lk.drop = 0;
    c->x[22] = (c->x[22] & ~4ull) | (request ? 4u : 0u);
}

// main+0xd8aa54, `cmp w10, #2` in the player's shield posture (an AS parameter set every frame, main+0xd8a9e0): w10 the
// AttentionReceiver's lock state with bit0 cleared (2: starting or locked, which picks the lock posture, the shield
// toward the target); x22 the Player component, its virtual-key hold mask at [+0x48] (ZL is bit 2, the posture's next
// test). Locked without ZL reads as unlocked, so the posture falls through to the ZL test and the shield stays down.
void lockon_posture(Cpu *c) {
    if ((uint32_t)c->x[10] != 2) return;
    uint64_t keys = LD64(c->x[22] + 0x48);
    if (keys && LD8(keys) & 4) return;
    FIRST_RUN("lockon_posture");
    c->x[10] = 0;
}
#endif
