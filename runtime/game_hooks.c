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
#endif
